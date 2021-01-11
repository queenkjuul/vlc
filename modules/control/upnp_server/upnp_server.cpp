/*****************************************************************************
 * upnp_server.cpp : UPnP server module
 *****************************************************************************
 * Copyright © 2019 VLC authors and VideoLAN
 *
 * Authors: Hamza Parnica <hparnica@gmail.com>
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

#include "upnp_server.h"

#include <upnp/ExtraHeaders.h>

#include <vlc_common.h>

#include <vlc_addons.h>
#include <vlc_cxx_helpers.hpp>
#include <vlc_interface.h>
#include <vlc_player.h>
#include <vlc_rand.h>
#include <vlc_stream_extractor.h>
#include <vlc_url.h>
#include "../stream_out/renderer_common.hpp"

#include <atomic>
#include <cstring>
#include <iostream>

#include "xml/Description.h"

#include "cds/cds.hpp"
#include "utils.hpp"

static void medialibrary_event_callback( void* p_data, const struct vlc_ml_event_t* p_event )
{
    intf_thread_t* p_intf = (intf_thread_t*)p_data;
    intf_sys_t* p_sys = p_intf->p_sys;

    switch ( p_event->i_type )
    {
    case VLC_ML_EVENT_MEDIA_ADDED:
    case VLC_ML_EVENT_MEDIA_UPDATED:
    case VLC_ML_EVENT_MEDIA_DELETED:
    case VLC_ML_EVENT_ARTIST_ADDED:
    case VLC_ML_EVENT_ARTIST_UPDATED:
    case VLC_ML_EVENT_ARTIST_DELETED:
    case VLC_ML_EVENT_ALBUM_ADDED:
    case VLC_ML_EVENT_ALBUM_UPDATED:
    case VLC_ML_EVENT_ALBUM_DELETED:
    case VLC_ML_EVENT_PLAYLIST_ADDED:
    case VLC_ML_EVENT_PLAYLIST_UPDATED:
    case VLC_ML_EVENT_PLAYLIST_DELETED:
    case VLC_ML_EVENT_GENRE_ADDED:
    case VLC_ML_EVENT_GENRE_UPDATED:
    case VLC_ML_EVENT_GENRE_DELETED:
        p_sys->i_update_id++;
        break;
    }
}

static bool cds_browse( UpnpActionRequest* p_request, intf_thread_t* p_intf )
{
    intf_sys_t* p_sys = p_intf->p_sys;

    const auto* action_rq = UpnpActionRequest_get_ActionRequest( p_request );

    const char* psz_object_id = xml_getChildElementValue( (IXML_Element*)action_rq, "ObjectID" );
    if ( psz_object_id == nullptr )
        return false;

    const char* psz_browse_flag =
        xml_getChildElementValue( (IXML_Element*)action_rq, "BrowseFlag" );
    const uint32_t u_starting_index =
        strtoull( xml_getChildElementValue( (IXML_Element*)action_rq, "StartingIndex" ), NULL, 10 );
    const uint32_t u_requested_count =
        strtoul( xml_getChildElementValue( (IXML_Element*)action_rq, "RequestedCount" ), NULL, 10 );

    xml::Document result;
    xml::Element didl_lite = result.create_element( "DIDL-Lite" );

    // Standard upnp attributes
    didl_lite.set_attribute( "xmlns", "urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" );
    didl_lite.set_attribute( "xmlns:dc", "http://purl.org/dc/elements/1.1/" );
    didl_lite.set_attribute( "xmlns:upnp", "urn:schemas-upnp-org:metadata-1-0/upnp/" );

    const std::string id = psz_object_id;
    unsigned obj_idx;
    cds::Object::ExtraId extra_id;
    std::tie( obj_idx, extra_id ) = cds::parse_id( id );

    std::string str_nb_returned;
    std::string str_total_matches;
    const cds::Object& obj = *p_sys->hierarchy[obj_idx];
    if ( strcmp( psz_browse_flag, "BrowseDirectChildren" ) == 0 && obj.type == cds::Object::Type::Container)
    {
        const auto browse_stats = static_cast<const cds::Container&>( obj ).browse_direct_children(
            didl_lite, { u_starting_index, u_requested_count }, extra_id );
        str_nb_returned = std::to_string( browse_stats.result_count );
        str_total_matches = std::to_string( browse_stats.total_matches );
    }
    else if ( strcmp( psz_browse_flag, "BrowseMetadata" ) == 0 )
    {
        didl_lite.add_child( obj.browse_metadata( result, extra_id ) );
        str_nb_returned = "1";
        str_total_matches = "1";
    }

    result.set_entry( std::move( didl_lite ) );

    const char* action_name = UpnpActionRequest_get_ActionName_cstr( p_request );

    const auto up_reponse_str = result.to_wrapped_cstr();
    const auto str_update_id = std::to_string( p_sys->i_update_id.load() );

    auto* p_answer = UpnpActionRequest_get_ActionResult( p_request );

    static constexpr char service_type[] = UPNP_SERVICE_TYPE("ContentDirectory");
    UpnpAddToActionResponse( &p_answer, action_name, service_type, "Result", up_reponse_str.get() );
    UpnpAddToActionResponse( &p_answer, action_name, service_type, "NumberReturned", str_nb_returned.c_str() );
    UpnpAddToActionResponse( &p_answer, action_name, service_type, "TotalMatches", str_total_matches.c_str() );
    UpnpAddToActionResponse( &p_answer, action_name, service_type, "UpdateID", str_update_id.c_str() );

    UpnpActionRequest_set_ActionResult( p_request, p_answer );
    msg_Dbg( p_intf, "server: sending response to client: \n%s", up_reponse_str.get() );

    return true;
}

static void handle_action_request( UpnpActionRequest* p_request, intf_thread_t* p_intf )
{
    intf_sys_t* p_sys = p_intf->p_sys;


    IXML_Document* action_result = nullptr;
    const char* service_id = UpnpActionRequest_get_ServiceID_cstr( p_request );
    const char* action_name = UpnpActionRequest_get_ActionName_cstr( p_request );
    msg_Dbg( p_intf, "server: received action request \"%s\" for service \"%s\"", action_name,
             service_id );
    if ( strcmp( service_id, CMS_ID ) == 0 )
    {
        if ( strcmp( action_name, "GetProtocolInfo" ) == 0 )
        {
            UpnpAddToActionResponse( &action_result, action_name, CMS_ID, "Source",
                                     "http-get:*:video/mp4:*,http-get:*:video/x-matroska:*" );
            UpnpAddToActionResponse( &action_result, action_name, CMS_ID, "Sink", "" );
            UpnpActionRequest_set_ActionResult( p_request, action_result );
        }
        else if (strcmp( action_name, "GetCurrentConnectionIDs" ) == 0)
        {
            UpnpAddToActionResponse( &action_result, action_name, CMS_ID, "ConnectionIDs", "" );
            UpnpActionRequest_set_ActionResult( p_request, action_result );
        }
    }

    else if ( strcmp( service_id, CDS_ID ) == 0 )
    {
        if ( strcmp( action_name, "Browse" ) == 0 )
        {
            if ( !cds_browse( p_request, p_intf ) )
                msg_Err( p_intf, "server: failed to respond to browse action request" );
        }
        else if ( strcmp( action_name, "GetSearchCapabilities" ) == 0 )
        {
            UpnpAddToActionResponse( &action_result, action_name, CDS_ID, "SearchCaps", "" );
            UpnpActionRequest_set_ActionResult( p_request, action_result );
        }
        else if ( strcmp( action_name, "GetSortCapabilities" ) == 0 )
        {
            UpnpAddToActionResponse( &action_result, action_name, CDS_ID, "SortCaps", "" );
            UpnpActionRequest_set_ActionResult( p_request, action_result );
        }
        else if ( strcmp( action_name, "GetSystemUpdateID" ) == 0 )
        {
            char* psz_update_id;
            if ( asprintf( &psz_update_id, "%d", p_sys->i_update_id.load() ) == -1 )
                return;
            auto up_update_id = vlc::wrap_cptr( psz_update_id );
            UpnpAddToActionResponse( &action_result, action_name, CDS_ID, "Id", psz_update_id );
            UpnpActionRequest_set_ActionResult( p_request, action_result );
        }
    }
}

static int Callback( Upnp_EventType event_type, const void* p_event, void* p_cookie )
{
    intf_thread_t* p_intf = (intf_thread_t*)p_cookie;

    switch ( event_type )
    {
    case UPNP_CONTROL_ACTION_REQUEST:
    {
        msg_Warn( p_intf, "server: action request" );
        // We need to const_cast here because the upnp callback has to take a const void* for the
        // event data even if the sdk also expect us to modify it sometimes (in the case of a upnp
        // response for example)
        auto* rq =
            const_cast<UpnpActionRequest*>( static_cast<const UpnpActionRequest*>( p_event ) );
        handle_action_request( rq, p_intf );
    }
    break;
    case UPNP_CONTROL_GET_VAR_REQUEST:
        msg_Warn( p_intf, "server: var request" );
        break;

    case UPNP_EVENT_SUBSCRIPTION_REQUEST:
        msg_Warn( p_intf, "server: sub request" );
        break;

    default:
        msg_Err( p_intf, "server: unhandled event: %d", event_type );
        return UPNP_E_INVALID_ACTION;
    }

    return UPNP_E_SUCCESS;
}

// UPNP Callbacks

struct FileInfo
{
    const TranscodeProfile* transcode_profile;
    std::string extension;
    vlc_ml_media_t* media;
};

int parse_url( const char* url, intf_sys_t* p_sys, FileInfo* file_info )
{
    std::stringstream ss( url );

    std::string token;
    const TranscodeProfile* profile = nullptr;

    std::getline( ss, token, '/' );
    if ( !token.empty() )
        return VLC_EGENERIC;

    std::getline( ss, token, '/' );
    if ( token != "media" )
        return VLC_EGENERIC;
    std::getline( ss, token, '/' );
    if ( token != "native" )
    {
        for ( unsigned i = 0; i < p_sys->transcode_profiles.u_size; ++i )
        {
            const TranscodeProfile& current_profile = p_sys->transcode_profiles.p_profiles[i];
            if ( token == current_profile.psz_name )
            {
                profile = &current_profile;
                break;
            }
        }
        if ( profile == nullptr )
            return VLC_EGENERIC;
    }

    std::getline( ss, token );
    const auto extension_idx = token.find( '.' );
    if ( extension_idx == std::string::npos )
        return VLC_EGENERIC;

    int media_id;
    try
    {
        media_id = std::stoi( token.substr( 0, extension_idx ) );
    }
    catch ( const std::invalid_argument& )
    {
        return VLC_EGENERIC;
    }

    vlc_ml_media_t* media = vlc_ml_get_media( p_sys->p_ml, media_id );
    if ( media == nullptr )
        return VLC_EGENERIC;
    *file_info = { profile, token.substr( extension_idx + 1 ), media };
    return VLC_SUCCESS;
}

static int
getinfo_cb( const char* fname, UpnpFileInfo* info, intf_thread_t* intf, const bool** transcoding )
{
    msg_Dbg( intf, "GetInfo callback on: %s", fname );
    FileInfo file_info;
    if ( parse_url( fname, intf->p_sys, &file_info ) != VLC_SUCCESS )
        return UPNP_E_FILE_NOT_FOUND;

    UpnpFileInfo_set_IsReadable( info, true );
    UpnpFileInfo_set_IsDirectory( info, false );

    const bool is_transcoding = file_info.transcode_profile != nullptr;
    *transcoding = new bool( is_transcoding );

    const std::string mime_type = utils::get_mimetype(
        file_info.media->i_type,
        (transcoding && file_info.extension != "webm") ? "mkv" : file_info.extension );
    UpnpFileInfo_set_ContentType( info, mime_type.c_str() );

    const auto &media_files = utils::get_media_files(*file_info.media, VLC_ML_FILE_TYPE_MAIN);
    if (media_files.empty())
    {
      vlc_ml_release( file_info.media );
      return UPNP_E_FILE_NOT_FOUND;
    }

    const vlc_ml_file_t &first = media_files.front();

    const int64_t size = is_transcoding ? -1 : first.i_size;
    UpnpFileInfo_set_FileLength( info, size );

    UpnpFileInfo_set_LastModified( info, first.i_last_modification_date );

    auto *head = const_cast<UpnpListHead*>(UpnpFileInfo_get_ExtraHeadersList( info ));

    for ( UpnpListIter iter = UpnpListBegin( head ); iter != UpnpListEnd( head );
          iter = UpnpListNext( head, iter ) )
    {
        const char* name = UpnpExtraHeaders_get_name_cstr( (UpnpExtraHeaders*)iter );
        msg_Info( intf, "extra hdr: %s", name );
    }

   // {
   // UpnpExtraHeaders* h = UpnpExtraHeaders_new();
   // UpnpExtraHeaders_set_resp( h, "transferMode.dlna.org: Streaming" );
   // UpnpListInsert( head, UpnpListEnd( head ),
   //                 const_cast<UpnpListHead*>( UpnpExtraHeaders_get_node( h ) ) );
   // }
   // {
   // UpnpExtraHeaders* h = UpnpExtraHeaders_new();
   // UpnpExtraHeaders_set_resp( h, "contentFeatures.dlna.org: DLNA.ORG_PN=DLNA.ORG_PN=AVC_MP4_EU;DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01200000000000000000000000000000" );
   // UpnpListInsert( head, UpnpListEnd( head ),
   //                 const_cast<UpnpListHead*>( UpnpExtraHeaders_get_node( h ) ) );
   // }

    vlc_ml_release( file_info.media );

    return UPNP_E_SUCCESS;
}

struct FileHandler
{
    struct vlc_object_t obj;
    Fifo fifo;
    vlc_player_t* p_player;

    static void release( FileHandler* fhandler )
    {
        vlc_player_Delete( fhandler->p_player );
        block_FifoRelease( fhandler->fifo.queue );
        var_SetAddress( &fhandler->obj, Fifo::VAR_NAME, nullptr );
        vlc_object_delete( &fhandler->obj );
    }
};
vlc_object_cast( FileHandler );


static const char DEFAULT_MUXER[] = "avformat{mux=matroska,options={live=1},reset-ts}";
static const char DEFAULT_MUXER_WEBM[] = "avformat{mux=webm,options={live=1},reset-ts}";

static UpnpWebFileHandle
open_cb( const char* fname, enum UpnpOpenFileMode, intf_thread_t* intf, const bool* transcoding )
{
    msg_Dbg(intf, "Opening: %s", fname);
    assert( transcoding );

    FileInfo file_info;
    if ( parse_url( fname, intf->p_sys, &file_info ) != VLC_SUCCESS )
        return nullptr;

    const auto& main_files = utils::get_media_files(*file_info.media, VLC_ML_FILE_TYPE_MAIN);
    if ( main_files.empty() )
    {
        vlc_ml_release( file_info.media );
        return nullptr;
    }

    const vlc_ml_file_t& main_file = main_files.front();
    const char* psz_mrl = main_file.psz_mrl;

    if ( *transcoding )
    {
        FileHandler* fhandler = static_cast<FileHandler*>(
            vlc_object_create( VLC_OBJECT( intf ), sizeof( *fhandler ) ) );

        fhandler->fifo = {};
        fhandler->p_player =
            vlc_player_New( VLC_OBJECT( fhandler ), VLC_PLAYER_LOCK_NORMAL, nullptr, nullptr );

        input_item_t* item = vlc_ml_get_input_item_by_mrl( intf->p_sys->p_ml, psz_mrl );

        if (item == nullptr || fhandler->p_player == nullptr)
        {
          FileHandler::release(fhandler);
          return nullptr;
        }

        const auto video_tracks =
            utils::get_media_tracks( *file_info.media, VLC_ML_TRACK_TYPE_VIDEO );
        video_format_t fmt;
        video_format_Init(&fmt, VLC_CODEC_I420);
        if (video_tracks.empty())
        {
          FileHandler::release(fhandler);
          return nullptr;
        }
        assert(video_tracks.size() ==  1);

        const vlc_ml_media_track_t& vtrack = video_tracks[0];
        video_format_Setup( &fmt, VLC_CODEC_I420, vtrack.v.i_width, vtrack.v.i_height,
                            vtrack.v.i_width, vtrack.v.i_height, 1,
                            1 );

        vlc_fourcc_t codec = 0;
        std::stringstream transcode_line;
        transcode_line << "sout=#transcode{";
        try {
        transcode_line << vlc_sout_renderer_GetVcodecOption(
            VLC_OBJECT( intf ), file_info.transcode_profile->codecs_fallbacks, &codec, &fmt,
            file_info.transcode_profile->i_conversion_quality ) /*<< "height=1080"*/;
        } catch (const std::domain_error& e) {
          msg_Err(intf, "No encoder was fitting the transcode chain: %s", e.what());
          FileHandler::release(fhandler);
          return nullptr;
        }
        transcode_line << "}";

        const char* mux = file_info.extension == "webm" ? DEFAULT_MUXER_WEBM : DEFAULT_MUXER;
        transcode_line << ":std{access=upnp-out,mux=" << mux << "}";

        const auto transcode_line_str = transcode_line.str();
        int res =
            input_item_AddOption( item, transcode_line_str.c_str(), VLC_INPUT_OPTION_TRUSTED );

        if (res != VLC_SUCCESS)
        {
          msg_Err(intf, "Can't append transcode chain to the player");
          FileHandler::release(fhandler);
          return nullptr;
        }

        msg_Info( intf, "Transcoding '%s' with profile '%s'",
                  file_info.media->psz_title, file_info.transcode_profile->psz_name );
        msg_Dbg( intf, "Transcode chain: %s", transcode_line_str.c_str() );

        var_Create( VLC_OBJECT( fhandler ), Fifo::VAR_NAME, VLC_VAR_ADDRESS );
        var_SetAddress( VLC_OBJECT( fhandler ), Fifo::VAR_NAME, &fhandler->fifo );

        vlc_player_Lock( fhandler->p_player );
        vlc_player_SetCurrentMedia( fhandler->p_player, item );
        vlc_player_Start( fhandler->p_player );
        vlc_player_Unlock( fhandler->p_player );

        vlc_ml_release( file_info.media );
        input_item_Release(item);

        return fhandler;
    }

    stream_t* ret = vlc_stream_NewMRL( VLC_OBJECT( intf ), psz_mrl );
    if (ret == nullptr)
    {
      msg_Err(intf, "Failed to create stream extractor to %s", psz_mrl);
    }
    vlc_ml_release( file_info.media );
    return ret;
}

int read_cb(
    UpnpWebFileHandle fileHnd, uint8_t buf[], size_t buflen, intf_thread_t* intf, const bool* transcoding )
{
    assert( fileHnd );
    assert( transcoding );

    size_t bytes_read;
    if ( *transcoding )
    {
        Fifo& fifo = static_cast<FileHandler*>( fileHnd )->fifo;
        bytes_read = fifo.read(buf, buflen);
    }
    else
    {
        stream_t* stream = static_cast<stream_t*>( fileHnd );
        bytes_read = vlc_stream_Read( stream, buf, buflen );
    }

    msg_Dbg(intf, "http read callback, %zub requested %zub returned", buflen, bytes_read);

    return bytes_read;
}

int seek_cb(
    UpnpWebFileHandle fileHnd, off_t offset, int origin, intf_thread_t* intf, const bool* transcoding )
{
    assert( fileHnd );
    assert( transcoding );
    msg_Info(intf, "http seek callback offset: %ld origin: %d", offset, origin);

    // TODO implement seek when transcoding on the fly (not a priority)
    if ( *transcoding )
    {
        return -1;
    }

    stream_t* stream = static_cast<stream_t*>( fileHnd );

    uint64_t real_offset;
    switch ( origin )
    {
    case SEEK_CUR:
        real_offset = vlc_stream_Tell( stream ) + offset;
        break;
    case SEEK_END:
        if ( vlc_stream_GetSize( stream, &real_offset ) != VLC_SUCCESS )
        {
            return -1;
        }
        real_offset += offset;
        break;
    case SEEK_SET:
        real_offset = offset;
        break;
    default:
        return -1;
    }

    msg_Info(intf, "moving file offset to %ld", offset);
    return vlc_stream_Seek( stream, real_offset );
}

int close_cb( UpnpWebFileHandle fileHnd, intf_thread_t* intf, const bool* transcoding )
{
    assert( fileHnd );
    assert( transcoding );
    msg_Dbg(intf, "http close callback");

    bool is_transcoding = *transcoding;
    delete transcoding;

    if ( is_transcoding )
    {
        FileHandler* handler = static_cast<FileHandler*>( fileHnd );

        FileHandler::release(handler);

        return UPNP_E_SUCCESS;
    }

    stream_t* stream = static_cast<stream_t*>( fileHnd );

    vlc_stream_Delete( stream );
    return UPNP_E_SUCCESS;
}

static xml::Document make_server_identity(const char *uuid) {
  xml::Document ret;

  const auto icon_elem = [&ret]( const char* url, const char* width,
                                 const char* height ) -> xml::Element {
      return ret.create_element( "icon",
          ret.create_element( "mimetype", ret.create_text_node( "image/png" ) ),
          ret.create_element( "width", ret.create_text_node( width ) ),
          ret.create_element( "height", ret.create_text_node( height ) ),
          ret.create_element( "depth", ret.create_text_node( "8" ) ),
          ret.create_element( "url", ret.create_text_node( url ) ) );
  };

  const auto service_elem = [&ret]( const char* service ) -> xml::Element {
    const auto type = std::string("urn:schemas-upnp-org:service:") + service + ":1";
    const auto id = std::string("urn:upnp-org:serviceId:") + service;
    const auto scpd_url = std::string("/") + service + ".xml";
    const auto control_url = std::string("/") + service + "/Control";
    const auto event_url = std::string("/") + service + "/Event";
      return ret.create_element( "service",
          ret.create_element( "serviceType", ret.create_text_node( type.c_str() ) ),
          ret.create_element( "serviceId", ret.create_text_node( id.c_str() ) ),
          ret.create_element( "SCPDURL", ret.create_text_node( scpd_url.c_str() ) ),
          ret.create_element( "controlURL", ret.create_text_node( control_url.c_str() ) ),
          ret.create_element( "eventSubURL", ret.create_text_node( event_url.c_str() ) ) );
  };

  const std::string url = utils::get_server_url();

  const auto uuid_attr = std::string("uuid:") + uuid;

  xml::Element root = ret.create_element("root",
      ret.create_element("specVersion",
        ret.create_element("major", ret.create_text_node( "1")),
        ret.create_element("minor", ret.create_text_node( "0"))
      ),
      ret.create_element("device",
        ret.create_element("deviceType", ret.create_text_node("urn:schemas-upnp-org:device:MediaServer:1")),
        ret.create_element("presentationUrl", ret.create_text_node(url.c_str())),
        ret.create_element("friendlyName", ret.create_text_node("VLC Media Server")),
        ret.create_element("manufacturer", ret.create_text_node("VideoLAN")),
        ret.create_element("manufacturerURL", ret.create_text_node("http://videolan.org")),
        ret.create_element("modelDescription", ret.create_text_node("VLC Upnp Media Server")),
        ret.create_element("modelName", ret.create_text_node("VLC")),
        ret.create_element("modelNumber", ret.create_text_node(VERSION_MESSAGE)),
        ret.create_element("modelURL", ret.create_text_node("http://videolan.org/vlc/")),
        ret.create_element("serialNumber", ret.create_text_node("1")),
        ret.create_element("UDN", ret.create_text_node(uuid_attr.c_str())),
        ret.create_element("iconList",
          icon_elem("/vlc512x512.png", "512", "512")
        ),
        ret.create_element("serviceList",
          service_elem("ConnectionManager"),
          service_elem("ContentDirectory")/*,
          service_elem("AVTransport")*/
        )
      ),
      ret.create_element("URLBase", ret.create_text_node(url.c_str()))
  );
  root.set_attribute("xmlns", "urn:schemas-upnp-org:device-1-0");

  ret.set_entry(std::move(root));
  return ret;
}

static bool init_upnp( intf_thread_t* p_intf )
{
    intf_sys_t* p_sys = p_intf->p_sys;

    int i_res;

    addon_uuid_t uuid;
    vlc_rand_bytes( uuid, sizeof( uuid ) );
    p_sys->psz_uuid = addons_uuid_to_psz( &uuid );

    p_sys->up_uuid = std::unique_ptr<char>{ p_sys->psz_uuid };

    msg_Info( p_intf, "upnp server enabled on %s:%d", UpnpGetServerIpAddress(), UpnpGetServerPort() );

    i_res = UpnpEnableWebserver( true );
    if ( i_res != UPNP_E_SUCCESS )
    {
        msg_Err( p_intf, "server: enabling webserver failed: %s", UpnpGetErrorMessage( i_res ) );
        return false;
    }

    const auto str_rootdir = utils::get_root_dir();

    i_res = UpnpSetWebServerRootDir( str_rootdir.c_str() );
    msg_Dbg(p_intf, "webserver root dir set to: \"%s\"", str_rootdir.c_str());
    if ( i_res != UPNP_E_SUCCESS )
    {
        msg_Err( p_intf, "server: setting webserver root dir failed: %s",
                 UpnpGetErrorMessage( i_res ) );
        UpnpEnableWebserver( false );
        return false;
    }

    const auto presentation_doc = make_server_identity(p_sys->up_uuid.get());
    const auto up_presentation_str = presentation_doc.to_wrapped_cstr();
    i_res = UpnpRegisterRootDevice2( UPNPREG_BUF_DESC, up_presentation_str.get(),
                                     strlen( up_presentation_str.get() ), 1, Callback, p_intf,
                                     &p_sys->p_device_handle );
    if ( i_res != UPNP_E_SUCCESS )
    {
        msg_Err( p_intf, "server: registration failed: %s", UpnpGetErrorMessage( i_res ) );
        UpnpEnableWebserver( false );
        return false;
    }

    UpnpAddVirtualDir( "/media", p_intf, nullptr );
    UpnpVirtualDir_set_GetInfoCallback( reinterpret_cast<VDCallback_GetInfo>( getinfo_cb ) );
    UpnpVirtualDir_set_OpenCallback( reinterpret_cast<VDCallback_Open>( open_cb ) );
    UpnpVirtualDir_set_ReadCallback( reinterpret_cast<VDCallback_Read>( read_cb ) );
    UpnpVirtualDir_set_SeekCallback( reinterpret_cast<VDCallback_Seek>( seek_cb ) );
    UpnpVirtualDir_set_CloseCallback( reinterpret_cast<VDCallback_Close>( close_cb ) );

    i_res = UpnpSendAdvertisement( p_sys->p_device_handle, 1800 );
    if ( i_res != UPNP_E_SUCCESS )
    {
        msg_Dbg( p_intf, "server: advertisement failed: %s", UpnpGetErrorMessage( i_res ) );
        UpnpUnRegisterRootDevice( p_sys->p_device_handle );
        UpnpEnableWebserver( false );
        return false;
    }

    p_sys->i_update_id = 0;

    return true;
}

namespace Server
{
int OpenServer( vlc_object_t* p_this )
{
    intf_thread_t* p_intf = (intf_thread_t*)p_this;

    intf_sys_t* p_sys = new ( std::nothrow ) intf_sys_t;
    if ( unlikely( p_sys == nullptr ) )
        return VLC_ENOMEM;
    p_intf->p_sys = p_sys;

    p_sys->p_ml = vlc_ml_instance_get( p_this );
    if ( !p_sys->p_ml )
    {
        msg_Err( p_intf, "server: cannot load server, medialibrary not initialized" );
        delete p_sys;
        return VLC_EGENERIC;
    }
    p_sys->p_ml_callback_handle =
        vlc_ml_event_register_callback( p_sys->p_ml, medialibrary_event_callback, p_intf );
    vlc_medialibrary_t* p_ml = p_sys->p_ml;
    p_sys->up_ml_callback_handle =
        std::unique_ptr<vlc_ml_event_callback_t, std::function<void( vlc_ml_event_callback_t* )>>{
            p_sys->p_ml_callback_handle, [p_ml]( vlc_ml_event_callback_t* p_ch ) {
                vlc_ml_event_unregister_callback( p_ml, p_ch );
            } };

    p_sys->p_upnp = UpnpInstanceWrapper::get( p_this );
    p_sys->up_upnp =
        std::unique_ptr<UpnpInstanceWrapper, std::function<void( UpnpInstanceWrapper* )>>{
            p_sys->p_upnp, []( UpnpInstanceWrapper* p_upnp ) { p_upnp->release(); } };

    static const TranscodeProfile profile_arr[] = {
//        { "high", CONVERSION_QUALITY_HIGH, {VLC_CODEC_H264, VLC_CODEC_VP8} },
        { "low", CONVERSION_QUALITY_LOW, {VLC_CODEC_H264, VLC_CODEC_VP8}},
    };
    p_sys->transcode_profiles = {
        sizeof( profile_arr ) / sizeof(TranscodeProfile),
        profile_arr,
    };

    p_sys->hierarchy = cds::init_hierarchy(*p_ml, p_sys->transcode_profiles);

    if ( !init_upnp( p_intf ) )
    {
        delete p_sys;
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

void CloseServer( vlc_object_t* p_this )
{
    intf_thread_t* intf = (intf_thread_t*)p_this;
    intf_sys_t* p_sys = intf->p_sys;

    UpnpUnRegisterRootDevice( p_sys->p_device_handle );
    UpnpEnableWebserver( false );

    delete p_sys;
}

namespace StreamOut
{

} // namespace StreamOut
} // namespace Server
