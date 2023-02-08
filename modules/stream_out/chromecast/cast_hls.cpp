#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../renderer_common.hpp"
#include "chromecast.h"


#include "../hls/hls.h"

#include <vlc_cxx_helpers.hpp>
#include <vlc_frame.h>
#include <vlc_sout.h>

#include <memory>
#include <algorithm>

#define SOUT_CFG_PREFIX "sout-chromecast-"

struct SoutStreamId
{
    es_format_t fmt;

    void *next_id = nullptr;
    vlc_frame_t *chain = nullptr;
    vlc_frame_t **chain_end = &chain;

    bool is_flushed = false;
};

struct SoutStreamSys
{
    std::unique_ptr<intf_sys_t> intf;
    std::unique_ptr<httpd_host_t, decltype( &httpd_HostDelete )> host;

    sout_stream_t *hls = nullptr;

    std::vector<std::unique_ptr<SoutStreamId>> tracks = {};

    bool is_video_supported = true;
    bool start_command_sent = false;
    bool error = false;
    bool is_flushing = false;

    vlc::threads::mutex lock = {};
    hls_pacer_t pacer = {false, VLC_TICK_INVALID};
};

static sout_stream_t *CreateSoutChain( sout_stream_t *stream, SoutStreamSys &sys )
{
    std::stringstream sstream;
    sstream << "transcode{acodec=aac}:"
            << "hls{is-http,"
            << ",path=" << sys.intf->getHttpStreamRoot() << "}";

    return sout_StreamChainNew( VLC_OBJECT( stream ), sstream.str().c_str(), nullptr );
}

static void *Add( sout_stream_t *stream, const es_format_t *fmt )
{
    SoutStreamSys *sys = static_cast<SoutStreamSys *>( stream->p_sys );

    //if (fmt->i_cat != VIDEO_ES)
    //{
    //    return nullptr;
    //}

    printf( "ADD %d\n", fmt->i_id );
    if ( !sys->is_video_supported && fmt->i_cat != AUDIO_ES )
    {
        return nullptr;
    }

    std::unique_ptr<SoutStreamId> id;
    try
    {
        id = std::make_unique<SoutStreamId>();
    }
    catch (const std::bad_alloc &)
    {
        return nullptr;
    }

    if ( sys->hls == nullptr )
    {
        sys->hls = CreateSoutChain( stream, *sys );
        if ( unlikely( sys->hls == nullptr ) )
        {
            return nullptr;
        }
    }
    id->next_id = sout_StreamIdAdd( sys->hls, fmt );
    if ( id->next_id == nullptr )
    {
        return nullptr;
    }

    if ( es_format_Copy( &id->fmt, fmt ) == VLC_EGENERIC )
    {
        return nullptr;
    }

    void *ret = id.get();
    sys->tracks.push_back(std::move(id));

    return ret;
}

static void RemoveTrack(std::vector<std::unique_ptr<SoutStreamId>> &tracks,
                        const SoutStreamId *track)
{
    auto track_idx = std::find_if(std::begin(tracks), std::end(tracks),
                                  [track](const auto &it) { return track == it.get(); });

    if (track_idx != std::end(tracks))
    {
        tracks.erase(track_idx);
    }
}

static void Del( sout_stream_t *stream, void *id )
{
    auto *sys = static_cast<SoutStreamSys *>( stream->p_sys );
    auto *sys_id = static_cast<SoutStreamId *>( id );

    if ( sys->hls != nullptr && sys_id->next_id != nullptr )
        sout_StreamIdDel( sys->hls, sys_id->next_id );

    //sys->GetTrackFromFmtCat( sys_id->fmt.i_cat ) = nullptr;

    es_format_Clean( &sys_id->fmt );

    RemoveTrack(sys->tracks, sys_id);
}

static int Send( sout_stream_t *stream, void *id, vlc_frame_t *frame )
{
    auto *sys = static_cast<SoutStreamSys *>( stream->p_sys );
    auto *sys_id = static_cast<SoutStreamId *>( id );

    if (sys_id->fmt.i_cat == SPU_ES)
    {
        printf("SUB ARRIVED: %.*s\n" , frame->i_buffer, frame->p_buffer);
    }
    if (sys->is_flushing)
    {
        sys->is_flushing = false;
    }

    //printf( "SEND %d\n", sys_id->fmt.i_id );
    // if ( sys->error )
    //{
    //     vlc_frame_ChainRelease( sys_id->chain );
    //     return VLC_EGENERIC;
    // }

    assert( sys->hls != nullptr );

    // FIXME handle err
    const int result = sout_StreamIdSend( sys->hls, sys_id->next_id, frame );
    if ( result != VLC_SUCCESS )
        return result;

    const auto hls_segcount = var_GetInteger( sys->hls->p_next, "seg-count" );
    if ( hls_segcount >= 2 && !sys->start_command_sent )
    {
        printf( "START\n" );
        sys->intf->setHasInput( "application/vnd.apple.mpegurl" );
        sys->pacer.is_enabled = true;
        sys->start_command_sent = true;
    }
    return VLC_SUCCESS;
}

static void Flush( sout_stream_t *stream, void *id )
{
    auto *sys = static_cast<SoutStreamSys *>( stream->p_sys );

    printf("FLUSH\n");

    if (sys->is_flushing)
    {
        return;
    }

    sys->is_flushing = true;

    sys->pacer.is_enabled = false;

    sys->intf->requestPlayerStop();
    sys->start_command_sent = false;


    for (auto &track : sys->tracks)
    {
        sout_StreamIdDel(sys->hls, track->next_id);
    }
    sout_StreamChainDelete(sys->hls, nullptr);

    //TODO err.
    sys->hls = CreateSoutChain(stream , *sys);
    for (auto &track : sys->tracks)
    {
        track->next_id = sout_StreamIdAdd(sys->hls, &track->fmt);
    }
}

static void SetPCR( sout_stream_t *stream, vlc_tick_t pcr )
{
    auto *sys = static_cast<SoutStreamSys *>( stream->p_sys );
    //printf( "SetPCR %ld\n", pcr );

    if ( sys->error )
    {
        return;
    }

    if ( sys->hls != nullptr )
    {
        sout_StreamSetPCR( sys->hls, pcr );
    }
}

static int Open( vlc_object_t *obj )
{
    sout_stream_t *stream = container_of( obj, sout_stream_t, obj );

    static const char *const sout_options[] = { "ip", "port", "http-port", "video", nullptr };
    config_ChainParse( stream, SOUT_CFG_PREFIX, sout_options, stream->p_cfg );

    const auto ip = vlc::wrap_cptr( var_GetNonEmptyString( stream, SOUT_CFG_PREFIX "ip" ) );
    if ( unlikely( ip == nullptr ) )
    {
         msg_Err( obj, "missing Chromecast IP address" );
         return VLC_EGENERIC;
    }

    int device_port = 0; // default
    int local_server_port = var_InheritInteger( stream, SOUT_CFG_PREFIX "http-port" );

    var_Create( stream, "http-port", VLC_VAR_INTEGER );
    var_SetInteger( stream, "http-port", local_server_port );
    var_Create( stream, "http-host", VLC_VAR_STRING );
    var_SetString( stream, "http-host", "" );

    auto host = vlc::wrap_cptr( vlc_http_HostNew( obj ), &httpd_HostDelete );
    if ( unlikely( host == nullptr ) )
        return VLC_ENOMEM;

    var_Create( stream, "httpd-host", VLC_VAR_ADDRESS );
    var_SetAddress( stream, "httpd-host", host.get() );

    std::unique_ptr<intf_sys_t> intf = nullptr;
     try
     {
         intf = std::make_unique<intf_sys_t>( obj, local_server_port, ip.get(), device_port,
                                              host.get() );
     }
     catch ( const std::runtime_error &err )
     {
         msg_Err( obj, "cannot load the Chromecast controller (%s)", err.what() );
         return VLC_EGENERIC;
     }
     catch ( const std::bad_alloc & )
     {
         return VLC_ENOMEM;
     }

    stream->p_sys = new ( std::nothrow ) SoutStreamSys{ std::move( intf ), std::move( host ) };
    if ( unlikely( stream->p_sys == nullptr ) )
        return VLC_ENOMEM;

    vlc_object_t *input = vlc_object_parent(vlc_object_parent(obj));
    var_Create(input, "hls-pacer", VLC_VAR_ADDRESS);
    var_SetAddress(input,
                   "hls-pacer",
                   &static_cast<SoutStreamSys *>(stream->p_sys)->pacer);

    static const struct sout_stream_operations ops = {
        Add, Del, Send, nullptr, Flush, SetPCR,
    };
    stream->ops = &ops;

    return VLC_SUCCESS;
}

static void Close( vlc_object_t *obj )
{
    sout_stream_t *stream = container_of( obj, sout_stream_t, obj );
    auto *sys = static_cast<SoutStreamSys *>( stream->p_sys );
    if (sys->hls != nullptr)
    {
        sout_StreamChainDelete(sys->hls, nullptr);
    }
    delete sys;
}

#define HTTP_PORT_TEXT N_( "HTTP port" )
#define HTTP_PORT_LONGTEXT                                                                         \
    N_( "This sets the HTTP port of the local server "                                             \
        "used to stream the media to the Chromecast." )

#define IP_ADDR_TEXT N_( "IP Address" )
#define IP_ADDR_LONGTEXT N_( "IP Address of the Chromecast." )
#define PORT_TEXT N_( "Chromecast port" )
#define PORT_LONGTEXT N_( "The port used to talk to the Chromecast." )

// clang-format off
vlc_module_begin()

    set_shortname( N_( "Chromecast" ) )
    set_description( N_( "Chromecast stream output" ) )
    set_capability( "sout output", 0 )
    add_shortcut( "chromecast" )
    set_subcategory( SUBCAT_SOUT_STREAM )
    set_callbacks( Open, Close )
    
    add_string( SOUT_CFG_PREFIX "ip", NULL, NULL, NULL )
        change_private()
    add_integer( SOUT_CFG_PREFIX "port", CHROMECAST_CONTROL_PORT, NULL, NULL )
        change_private()
    add_bool( SOUT_CFG_PREFIX "video", true, NULL, NULL )
        change_private()
    add_integer( SOUT_CFG_PREFIX "http-port", HTTP_PORT, HTTP_PORT_TEXT, HTTP_PORT_LONGTEXT )
    add_renderer_opts( SOUT_CFG_PREFIX )
vlc_module_end()
