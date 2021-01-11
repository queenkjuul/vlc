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

#include <vlc_common.h>

#include <vlc_addons.h>
#include <vlc_cxx_helpers.hpp>
#include <vlc_interface.h>
#include <vlc_player.h>
#include <vlc_rand.h>
#include <vlc_stream_extractor.h>
#include <vlc_url.h>

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
    vlc_medialibrary_t* p_ml = p_sys->p_ml;

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
    static const auto hierarchy = cds::init_hierarchy( *p_ml, p_sys->transcode_profiles );

    unsigned obj_idx;
    cds::Object::ExtraId extra_id;
    std::tie( obj_idx, extra_id ) = cds::parse_id( id );

    std::string str_nb_returned;
    std::string str_total_matches;
    const cds::Object& obj = hierarchy[obj_idx];
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

    UpnpAddToActionResponse( &p_answer, action_name, UPNP_SERVICE_TYPE("ContentDirectory"), "Result", up_reponse_str.get() );
    UpnpAddToActionResponse( &p_answer, action_name, UPNP_SERVICE_TYPE("ContentDirectory"), "NumberReturned", str_nb_returned.c_str() );
    UpnpAddToActionResponse( &p_answer, action_name, UPNP_SERVICE_TYPE("ContentDirectory"), "TotalMatches", str_total_matches.c_str() );
    UpnpAddToActionResponse( &p_answer, action_name, UPNP_SERVICE_TYPE("ContentDirectory"), "UpdateID", str_update_id.c_str() );

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
                                     "http-get:*:*:*" );
            UpnpAddToActionResponse( &action_result, action_name, CMS_ID, "Sink", "" );
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
getinfo_cb( const char* fname, UpnpFileInfo* info, const void* cookie, const bool** transcoding )
{
    const intf_thread_t* intf = static_cast<const intf_thread_t*>( cookie );

    FileInfo file_info;
    if ( parse_url( fname, intf->p_sys, &file_info ) != VLC_SUCCESS )
        return UPNP_E_FILE_NOT_FOUND;

    UpnpFileInfo_set_IsReadable( info, true );
    UpnpFileInfo_set_IsDirectory( info, false );

    const vlc_ml_file_t* main_file = nullptr;
    for ( auto i = 0u; i < file_info.media->p_files->i_nb_items; ++i )
    {
        const auto* file = &file_info.media->p_files->p_items[i];
        if ( file->i_type == VLC_ML_FILE_TYPE_MAIN )
        {
            main_file = file;
        }
    }

    const bool is_transcoding = file_info.transcode_profile != nullptr;
    *transcoding = new bool( is_transcoding );

    std::string mime_type =
        file_info.media->i_type == VLC_ML_MEDIA_TYPE_AUDIO ? "audio/" : "video/";
    if ( is_transcoding && file_info.transcode_profile->mux )
        mime_type += file_info.transcode_profile->mux;
    else
        mime_type += file_info.extension;
    UpnpFileInfo_set_ContentType( info, mime_type.c_str() );

    const int64_t size = is_transcoding ? -1 : main_file->i_size;
    UpnpFileInfo_set_FileLength( info, size );

    UpnpFileInfo_set_LastModified( info, main_file->i_last_modification_date );

    vlc_ml_release( file_info.media );

    return UPNP_E_SUCCESS;
}

struct FileHandler
{
    struct vlc_object_t obj;
    Fifo fifo;
    vlc_player_t* p_player;
};

vlc_object_cast( FileHandler );

static UpnpWebFileHandle
open_cb( const char* fname, enum UpnpOpenFileMode, intf_thread_t* intf, const bool* transcoding )
{
    msg_Dbg(intf, "Opening: %s", fname);
    assert( transcoding );

    FileInfo file_info;
    if ( parse_url( fname, intf->p_sys, &file_info ) != VLC_SUCCESS )
        return nullptr;

    const char* psz_mrl = nullptr;
    for ( auto i = 0u; i < file_info.media->p_files->i_nb_items; ++i )
    {
        const auto* file = &file_info.media->p_files->p_items[i];
        if ( file->i_type == VLC_ML_FILE_TYPE_MAIN )
        {
            psz_mrl = file->psz_mrl;
        }
    }
    if ( not psz_mrl )
        return nullptr;

    if ( *transcoding )
    {
        FileHandler* fhandler = static_cast<FileHandler*>(
            vlc_object_create( VLC_OBJECT( intf ), sizeof( *fhandler ) ) );

        fhandler->fifo = {};
        fhandler->p_player =
            vlc_player_New( VLC_OBJECT( fhandler ), VLC_PLAYER_LOCK_NORMAL, nullptr, nullptr );

        input_item_t* item = vlc_ml_get_input_item_by_mrl( intf->p_sys->p_ml, psz_mrl );

        const char* mux = file_info.transcode_profile->mux ? file_info.transcode_profile->mux
                                                           : file_info.extension.c_str();
        const std::string transcode_line =
            utils::make_transcode_line( *file_info.transcode_profile, *file_info.media ) +
            ":std{access=upnp-out,mux=" + mux + "}";
        int res = input_item_AddOption( item, transcode_line.c_str(), VLC_INPUT_OPTION_TRUSTED );
        assert( res == VLC_SUCCESS );

        msg_Info( intf, "Transcoding '%s' with profile '%s'",
                  file_info.media->psz_title, file_info.transcode_profile->psz_name );
        msg_Dbg( intf, "Transcode chain: %s", transcode_line.c_str() );

        var_Create( VLC_OBJECT( fhandler ), Fifo::VAR_NAME, VLC_VAR_ADDRESS );
        var_SetAddress( VLC_OBJECT( fhandler ), Fifo::VAR_NAME, &fhandler->fifo );

        vlc_player_Lock( fhandler->p_player );
        vlc_player_SetCurrentMedia( fhandler->p_player, item );
        vlc_player_Start( fhandler->p_player );
        vlc_player_Unlock( fhandler->p_player );

        vlc_ml_release( file_info.media );

        return fhandler;
    }

    stream_t* ret = vlc_stream_NewMRL( VLC_OBJECT( intf ), psz_mrl );
    vlc_ml_release( file_info.media );
    return ret;
}

int read_cb(
    UpnpWebFileHandle fileHnd, uint8_t buf[], size_t buflen, const void*, const bool* transcoding )
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
        bytes_read= vlc_stream_Read( stream, buf, buflen );
    }

    return bytes_read;
}

int seek_cb(
    UpnpWebFileHandle fileHnd, off_t offset, int origin, const void*, const bool* transcoding )
{
    assert( fileHnd );
    assert( transcoding );

    // TODO implement seek when transcoding on the fly (not a priority)
    if ( *transcoding )
    {
        return offset;
    }
    stream_t* stream = static_cast<stream_t*>( fileHnd );

    uint64_t real_offset;
    if ( origin == SEEK_CUR )
    {
        real_offset = vlc_stream_Tell( stream ) + offset;
    }
    else if ( origin == SEEK_END )
    {
        assert( vlc_stream_GetSize( stream, &real_offset ) == VLC_SUCCESS );
        real_offset += offset;
    }
    else
    {
        real_offset = offset;
    }
    return vlc_stream_Seek( stream, real_offset );
}

int close_cb( UpnpWebFileHandle fileHnd, const void*, const bool* transcoding )
{
    assert( fileHnd );
    assert( transcoding );

    bool is_transcoding = *transcoding;
    delete transcoding;

    if ( is_transcoding )
    {
        FileHandler* handler = static_cast<FileHandler*>( fileHnd );

        vlc_player_Delete( handler->p_player );

        block_FifoRelease( handler->fifo.queue );
        var_SetAddress( VLC_OBJECT( handler ), Fifo::VAR_NAME, nullptr );
        vlc_object_delete( VLC_OBJECT( handler ) );

        return 0;
    }

    stream_t* stream = static_cast<stream_t*>( fileHnd );

    vlc_stream_Delete( stream );
    return 0;
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
          service_elem("ContentDirectory")
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

    char* psz_rootdir;

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

    i_res =
        asprintf( &psz_rootdir, "%s/upnp_server/", config_GetSysPath( VLC_PKG_DATA_DIR, NULL ) );
    if ( i_res == -1 )
    {
        UpnpEnableWebserver( false );
        return false;
    }
    auto up_rootdir = vlc::wrap_cptr( psz_rootdir );

    i_res = UpnpSetWebServerRootDir( psz_rootdir );
    msg_Dbg(p_intf, "webserver root dir set to: \"%s\"", psz_rootdir);
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
        { "fullhd", VLC_ML_MEDIA_TYPE_VIDEO, { 1920, 1080 }, "mkv" },
        { "hd", VLC_ML_MEDIA_TYPE_VIDEO, { 1280, 720 }, "mkv" },
        { "wvga", VLC_ML_MEDIA_TYPE_VIDEO, { 854, 480 }, "mkv" },
    };
    p_sys->transcode_profiles = {
        sizeof( profile_arr ),
        profile_arr,
    };

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
