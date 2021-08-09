/*****************************************************************************
 * sout.c: HLS stream output module
 *****************************************************************************
 * Copyright (C) 2003-2011 VLC authors and VideoLAN
 *
 * Authors: Alaric Senat <dev.asenat@posteo.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#include "common.h"

#include <vlc_plugin.h>
#include <vlc_queue.h>
#include <vlc_sout.h>
#include <vlc_memstream.h>
#include <vlc_block.h>
#include <vlc_url.h>

#include <assert.h>

// stream-es-ids="1,3"
static const char *const sout_options[] = { NULL };


typedef struct
{
    httpd_host_t *httpd_host;

    struct vlc_memstream main_index_buffer;
    httpd_url_t *httpd_main_index_url;

    struct
    {
        hls_io *handle;
        vlc_mutex_t lock;
    } main_index;

    vlc_array_t medias;
    int stream_es_id;
} sout_stream_sys_t;

struct hls_media
{
    es_format_t fmt;
    sout_mux_t *mux;
    sout_input_t *mux_input;

    httpd_url_t *httpd_aco_index_url;
    hls_index index_file;

    httpd_host_t *httpd_host;
    vlc_queue_t segments_http_context;

    struct hls_sout_callbacks callbacks;
};

typedef struct segment_http_context
{
    httpd_url_t *url;
    struct segment_http_context *next;
} segment_http_context;

static int SegmentAdded( void *opaque, const sout_access_out_t *access, const hls_segment *segment )
{
    struct hls_media *obj = opaque;

    assert( obj->mux->p_access == access );

    vlc_url_t url;
    if ( vlc_UrlParse( &url, segment->psz_uri ) != VLC_SUCCESS )
        return VLC_EGENERIC;

    segment_http_context *context = calloc( 1, sizeof( *context ) );
    if ( unlikely( context == NULL ) )
        return VLC_ENOMEM;

    context->url = httpd_UrlNew( obj->httpd_host, url.psz_path, NULL, NULL );
    if ( unlikely( context->url == NULL ) )
    {
        free( context );
        vlc_UrlClean( &url );
        return VLC_ENOMEM;
    }
    httpd_UrlCatch( context->url, HTTPD_MSG_GET, (httpd_callback_t)url_segment_cb,
                    (void *)segment );

    vlc_queue_Enqueue( &obj->segments_http_context, context );

    vlc_UrlClean( &url );
    return VLC_SUCCESS;
}

static void IndexUpdated( void *opaque, const sout_access_out_t *access, const hls_io *index_io )
{
    struct hls_media *media = opaque;

    assert( media->mux->p_access == access );

    vlc_mutex_lock( &media->index_file.lock );
    media->index_file.handle = index_io;
    vlc_mutex_unlock( &media->index_file.lock );
}

static void
SegmentRemoved( void *opaque, const sout_access_out_t *access, const hls_segment *segment )
{
    struct hls_media *obj = opaque;

    assert( obj->mux->p_access == access );

    segment_http_context *context = vlc_queue_Dequeue( &obj->segments_http_context );

    // Should not be called when the queue is empty.
    assert( context );

    httpd_UrlDelete( context->url );
    free( context );

    (void)segment;
}

static char *hls_media_FormatURI( unsigned id )
{
    char *ret;
    if ( unlikely( asprintf( &ret, "/track-%u.m3u8", id ) == -1 ) )
        return NULL;
    return ret;
}

static httpd_url_t *hls_media_InitIndexUrl( httpd_host_t *host, unsigned id )
{
    char *url = hls_media_FormatURI(id);
    if ( unlikely( url == NULL ) )
        return NULL;
    httpd_url_t *ret = httpd_UrlNew( host, url, NULL, NULL );
    free( url );
    return ret;
}

static sout_access_out_t *
hls_media_InitAco( sout_stream_t *sout, unsigned object_index, const char *aco, const char *ext )
{
    char *url;
    if ( unlikely( asprintf( &url, "/track-%u/########.%s", object_index, ext ) ) == -1 )
        return NULL;

    char *cfg;
    if ( unlikely( asprintf(
             &cfg,
             "%s{seglen=10,delsegs=true,pace=true,numsegs=4,segment-url=http://192.168.0.23:8080%s}",
             aco, url ) ) == -1 )
    {
        free( url );
        return NULL;
    }

    sout_access_out_t *ret = sout_AccessOutNew( sout, cfg, url );
    free( url );
    free( cfg );
    return ret;
}

static struct hls_media *hls_media_New( sout_stream_t *sout, const es_format_t *fmt)
{
    assert( fmt->i_id >= 0 );

    struct hls_media *media = calloc( 1, sizeof( *media ) );
    if ( unlikely( media == NULL ) )
        return NULL;

    media->httpd_host = ((sout_stream_sys_t*)sout->p_sys)->httpd_host;
    media->httpd_aco_index_url = hls_media_InitIndexUrl( media->httpd_host, fmt->i_id );
    if ( unlikely( media->httpd_aco_index_url == NULL ) )
    {
        free( media );
        return NULL;
    }

    httpd_UrlCatch( media->httpd_aco_index_url, HTTPD_MSG_GET, (httpd_callback_t)url_index_cb,
                    (void *)&media->index_file );

    vlc_queue_Init( &media->segments_http_context, offsetof( segment_http_context, next ) );

    vlc_mutex_init( &media->index_file.lock );

    media->callbacks = ( struct hls_sout_callbacks ){ .sys = media,
                                                      .segment_added = SegmentAdded,
                                                      .segment_removed = SegmentRemoved,
                                                      .index_updated = IndexUpdated };

    const char *aco = fmt->i_cat == AUDIO_ES ? "pkd-audio-hls" : "hls" ;
    const char *ext = fmt->i_cat == AUDIO_ES ? "aac" : "ts" ;
    sout_access_out_t *access = hls_media_InitAco( sout, fmt->i_id, aco, ext );
    if ( unlikely( access == NULL ) )
        goto err;

    var_Create( access, HLS_SOUT_CALLBACKS_VAR, VLC_VAR_ADDRESS );
    var_SetAddress( access, HLS_SOUT_CALLBACKS_VAR, &media->callbacks );

    const char *mux = fmt->i_cat == AUDIO_ES ? "hlspack" : "ts{use-key-frames}";
    media->mux = sout_MuxNew( access, mux );
    if ( unlikely( media->mux == NULL ) )
    {
        sout_AccessOutDelete( access );
        goto err;
    }

    media->mux_input = sout_MuxAddStream( media->mux, fmt );

    es_format_Copy(&media->fmt, fmt);

    return media;
err:
    if ( media->httpd_aco_index_url )
        httpd_UrlDelete( media->httpd_aco_index_url );
    httpd_HostDelete( media->httpd_host );
    free( media );
    return NULL;
}

static void hls_media_Release( struct hls_media *media )
{
    sout_access_out_t *access = media->mux->p_access;

    sout_MuxDeleteStream( media->mux, media->mux_input );
    sout_MuxDelete( media->mux );

    sout_AccessOutDelete( access );
    // All segments should have been cleaned up after the access release.
    assert( vlc_queue_IsEmpty( &media->segments_http_context ) );

    httpd_UrlDelete( media->httpd_aco_index_url );
    httpd_HostDelete( media->httpd_host );

    free( media );
}

static bool fill_index_file(const sout_stream_sys_t *sys, struct vlc_memstream*stream)
{
    vlc_memstream_open( stream );

    static const char hls_hdr[] = "#EXTM3U\n";
    vlc_memstream_write( stream, hls_hdr, sizeof( hls_hdr ) - 1 );

    const struct hls_media *highest_media_priorities[] = {
        [VIDEO_ES] = NULL,
        [AUDIO_ES] = NULL,
        [SPU_ES] = NULL,
    };

    for ( size_t i = 0; i < sys->medias.i_count; ++i )
    {
        const struct hls_media *media = sys->medias.pp_elems[i];

        if ( media->fmt.i_id == sys->stream_es_id )
            continue;

        const struct hls_media *highest = highest_media_priorities[media->fmt.i_cat];
        const int current_prio = media->fmt.i_priority;
        if ( highest == NULL || highest->fmt.i_priority < current_prio )
            highest_media_priorities[media->fmt.i_cat] = media;
    }

    const struct hls_media *stream_inf = NULL;
    for ( size_t i = 0; i < sys->medias.i_count; ++i )
    {
        const struct hls_media *media = sys->medias.pp_elems[i];

        if ( media->fmt.i_id == sys->stream_es_id )
        {
            stream_inf = media;
            continue;
        }

        const es_format_t *fmt = &media->fmt;

        static const char *const media_types[] = {
            [VIDEO_ES] = "VIDEO",
            [AUDIO_ES] = "AUDIO",
            [SPU_ES] = "SUBTITLES",
        };
        const char *media_type = media_types[media->fmt.i_cat];

        char media_name[32];
        sprintf( media_name, "%d", fmt->i_id );

        const char *is_default_media = highest_media_priorities[fmt->i_cat] == media ? "YES" : "NO";
        vlc_memstream_printf( stream,
                              "#EXT-X-MEDIA:TYPE=%s,GROUP-ID=\"%s\",NAME=\"%s\",DEFAULT=%s,",
                              media_type, media_type, media_name, is_default_media );

        if ( fmt->psz_language )
            vlc_memstream_printf( stream, "LANGUAGE=\"%s\",", fmt->psz_language );

        char *uri = hls_media_FormatURI( fmt->i_id );
        if ( likely( uri != NULL ) )
        {
            vlc_memstream_printf( stream, "URI=\"%s\"\n", uri );
            free( uri );
        }
    }

    if (!stream_inf)
    {
        // The stream selected by the user isn't available.
        vlc_memstream_close(stream);
        free(stream->ptr);
        return false;
    }

    char avc_id[12];
    const uint8_t *extra = stream_inf->fmt.p_extra;
    sprintf(avc_id,"avc1.%02x%02x%02x", extra[5], extra[6], extra[7]);

    vlc_memstream_printf( stream, "#EXT-X-STREAM-INF:BANDWIDTH=%d,CODECS=\"avc1.42c01e,mp4a.40.2\"", /*stream_inf->fmt.i_bitrate */ 0);

    if ( highest_media_priorities[VIDEO_ES] )
        vlc_memstream_printf( stream, ",VIDEO=\"VIDEO\"" );
    if ( highest_media_priorities[AUDIO_ES] )
        vlc_memstream_printf( stream, ",AUDIO=\"AUDIO\"" );
    if ( highest_media_priorities[SPU_ES] )
        vlc_memstream_printf( stream, ",SUBTITLES=\"SUBTITLES\"" );

    char *uri = hls_media_FormatURI( stream_inf->fmt.i_id );
    vlc_memstream_printf( stream, "\n%s\n", uri );
    free( uri );

    vlc_memstream_close(stream);

    return true;
}

/*****************************************************************************
 * Module callbacks
 *****************************************************************************/

static void *Add( sout_stream_t *sout, const es_format_t *fmt )
{
    if ( fmt->i_cat == DATA_ES || fmt->i_cat == DATA_ES ||
         fmt->i_priority == ES_PRIORITY_NOT_SELECTABLE )
    {
        msg_Warn( sout, "Unsupported es." );
        return NULL;
    }

    if ( fmt->i_cat == AUDIO_ES && ( fmt->i_codec != VLC_CODEC_MP4A ) )
    {
        msg_Warn( sout, "Unsupported audio track codec." );
        return NULL;
    }

    struct hls_media *media = hls_media_New( sout, fmt );
    if ( media == NULL )
        return NULL;

    sout_stream_sys_t *sys = sout->p_sys;
    vlc_array_append( &sys->medias, media );

    free( sys->main_index_buffer.ptr );

    struct vlc_memstream stream;
    if ( !fill_index_file( sys, &stream ) )
        return media;

    printf ("INDEX: %s\n", stream.ptr);
    hls_io *io = hls_io_New( NULL, 0 );
    if ( unlikely( io == NULL ) )
    {
        free( stream.ptr );
        hls_media_Release( media );
        return NULL;
    }

    block_t *clone = block_Alloc( stream.length );
    memcpy( clone->p_buffer, stream.ptr, stream.length );

    io->ops.open( io );
    io->ops.consume_block( io, clone );
    io->ops.close( io );

    vlc_mutex_lock( &sys->main_index.lock );
    hls_io *current_handle = sys->main_index.handle;
    if ( current_handle )
    {
        current_handle->ops.release( current_handle );
        free( current_handle );
    }
    sys->main_index.handle = io;
    vlc_mutex_unlock( &sys->main_index.lock );

    // XXX: does that actually work lmao ?
    return media;
}

static void Del( sout_stream_t *sout, void *id )
{
    struct hls_media *media = id;

    sout_stream_sys_t *sys = sout->p_sys;
    const size_t idx = vlc_array_index_of_item( &sys->medias, media );
    vlc_array_remove( &sys->medias, idx );

    hls_media_Release( media );

    vlc_memstream_close( &sys->main_index_buffer );
    free(sys->main_index_buffer.ptr);

    //sys->main_index_buffer = new_index_file(sys);
}

static int Send( sout_stream_t *stream, void *id, block_t *buffer )
{
    struct hls_media *media = id;
    return sout_MuxSendBuffer( media->mux, media->mux_input, buffer );

    (void)stream;
}

static int Control( sout_stream_t *stream, int query, va_list args )
{
    (void)stream;
    // const sout_stream_sys_t *sys = stream->p_sys;

    switch ( query )
    {
    case SOUT_STREAM_IS_SYNCHRONOUS:
        *va_arg( args, bool * ) = false; // TODO
        break;

    default:
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

static void Flush( sout_stream_t *stream, void *id )
{
    struct hls_media *obj = id;
    sout_MuxFlush( obj->mux, obj->mux_input );
    (void)stream;
}

static const struct sout_stream_operations ops = {
    Add, Del, Send, Control, Flush,
};

int SoutOpen( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;

    sout_stream_sys_t *sys = stream->p_sys = calloc( 1, sizeof( *sys ) );
    if ( unlikely( sys == NULL ) )
        return VLC_ENOMEM;

    config_ChainParse( stream, SOUT_CFG_PREFIX, sout_options, stream->p_cfg );

    vlc_array_init( &sys->medias );

    sys->stream_es_id = 1;

    sys->httpd_host = vlc_http_HostNew( this );
    if ( unlikely( sys->httpd_host == NULL ) )
        goto err;

    sys->httpd_main_index_url = httpd_UrlNew( sys->httpd_host, "/stream.m3u8", NULL, NULL );
    if ( unlikely( sys->httpd_main_index_url == NULL ) )
        goto err;

    vlc_mutex_init(&sys->main_index.lock);

    httpd_UrlCatch( sys->httpd_main_index_url, HTTPD_MSG_GET, (httpd_callback_t)url_index_cb,
                    (void *)&sys->main_index );

    stream->ops = &ops;

    return VLC_SUCCESS;
err:
    if ( sys->httpd_host )
        httpd_HostDelete( sys->httpd_host );
    free( sys );
    return VLC_EGENERIC;
}

void SoutClose( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;

    httpd_UrlDelete( sys->httpd_main_index_url );
    httpd_HostDelete( sys->httpd_host );

    free( sys );
}
