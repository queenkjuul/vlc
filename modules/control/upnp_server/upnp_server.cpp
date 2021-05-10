/*****************************************************************************
 * upnp_server.cpp : UPnP server module
 *****************************************************************************
 * Copyright © 2019 VLC authors and VideoLAN
 *
 * Authors: Hamza Parnica <hparnica@gmail.com>
 *          Alaric Senat <dev.asenat@posteo.net>
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "upnp_server.hpp"

#include <vlc_common.h>

#include <vlc_addons.h>
#include <vlc_cxx_helpers.hpp>
#include <vlc_interface.h>
#include <vlc_player.h>
#include <vlc_rand.h>
#include <vlc_stream_extractor.h>
#include <vlc_dialog.h>
#include <vlc_url.h>

#include <atomic>
#include <cstring>
#include <iostream>

#include "FileHandler.hpp"
#include "cds/cds.hpp"
#include "cds/Container.hpp"
#include "utils.hpp"

#define CDS_ID "urn:upnp-org:serviceId:ContentDirectory"
#define CMS_ID "urn:upnp-org:serviceId:ConnectionManager"

#define UPNP_SERVICE_TYPE(service) "urn:schemas-upnp-org:service:" service ":1"

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
        p_sys->upnp_update_id++;
        break;
    }
}

static bool cds_browse( UpnpActionRequest* p_request, intf_thread_t* p_intf )
{
    intf_sys_t* p_sys = p_intf->p_sys;

    auto* action_rq =
        reinterpret_cast<IXML_Element*>( UpnpActionRequest_get_ActionRequest( p_request ) );

    const char* psz_object_id = xml_getChildElementValue( action_rq, "ObjectID" );
    if ( psz_object_id == nullptr )
        return false;

    const char* browse_flag = xml_getChildElementValue( action_rq, "BrowseFlag" );
    const char* starting_index = xml_getChildElementValue( action_rq, "StartingIndex" );
    const char* requested_count = xml_getChildElementValue( action_rq, "RequestedCount" );

    if ( browse_flag == nullptr || starting_index == nullptr || requested_count == nullptr )
        return false;

    cds::Container::BrowseParams browse_params{
        static_cast<uint32_t>(strtoul( starting_index, nullptr, 10 )),
        static_cast<uint32_t>(strtoul( requested_count, nullptr, 10 )),
    };

    xml::Document result;
    xml::Element didl_lite = result.create_element( "DIDL-Lite" );

    // Standard upnp attributes
    didl_lite.set_attribute( "xmlns", "urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" );
    didl_lite.set_attribute( "xmlns:dc", "http://purl.org/dc/elements/1.1/" );
    didl_lite.set_attribute( "xmlns:upnp", "urn:schemas-upnp-org:metadata-1-0/upnp/" );

    const std::string id = psz_object_id;
    unsigned obj_idx;
    cds::Object::ExtraId extra_id;

    try {
      std::tie( obj_idx, extra_id ) = cds::parse_id( id );
    } catch (const std::invalid_argument&e) {
      UpnpActionRequest_set_ErrCode(p_request, 500);
      return false;
    }

    // Extra header listing is only avaibale in 1.15, which is not yet released.
    // The feature can be found in this commit upstream: 508930f56cca885d82b94e6e7abf588d0e46faa4
    // TODO: Remove the `&& 0` when libupnp is bumped and the feature is available
#define UPNP_EXTRA_HEADERS_AVAILABLE UPNP_VERSION >= 11500 && 0

#if UPNP_EXTRA_HEADERS_AVAILABLE
    auto* extra_hdrs = (UpnpListHead*)UpnpActionRequest_get_ExtraHeadersList( p_request );
    const auto& client_profile =
        p_sys->profile_from_headers( extra_hdrs, UpnpActionRequest_get_Os_cstr( p_request ) );
#else
#warning "Upnp version too low to be able to support custom dlna behavior depending on clients user agent"
    const auto& client_profile = p_sys->default_profile();
    msg_Warn(p_intf, "Cant't defer client with user agent, libupnp version is too low (<1.15)");
#endif

    std::string str_nb_returned;
    std::string str_total_matches;
    const cds::Object& obj = *p_sys->obj_hierarchy[obj_idx];
    if ( strcmp( browse_flag, "BrowseDirectChildren" ) == 0 &&
         obj.type == cds::Object::Type::Container )
    {
        const auto browse_stats = static_cast<const cds::Container&>( obj ).browse_direct_children(
            didl_lite, browse_params, extra_id, client_profile);
        str_nb_returned = std::to_string( browse_stats.result_count );
        str_total_matches = std::to_string( browse_stats.total_matches );
    }
    else if ( strcmp( browse_flag, "BrowseMetadata" ) == 0 )
    {
        didl_lite.add_child( obj.browse_metadata( result, extra_id, client_profile ) );
        str_nb_returned = "1";
        str_total_matches = "1";
    }

    result.set_entry( std::move( didl_lite ) );

    const char* action_name = UpnpActionRequest_get_ActionName_cstr( p_request );

    const auto up_reponse_str = result.to_wrapped_cstr();
    const auto str_update_id = std::to_string( p_sys->upnp_update_id.load() );

    auto* p_answer = UpnpActionRequest_get_ActionResult( p_request );

    static constexpr char service_type[] = UPNP_SERVICE_TYPE( "ContentDirectory" );
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
    const auto client_addr = utils::addr_to_string(UpnpActionRequest_get_CtrlPtIPAddr( p_request ));
    msg_Dbg( p_intf, "server: received action request \"%s\" for service \"%s\" from %s",
             action_name, service_id, client_addr.c_str() );
    if ( strcmp( service_id, CMS_ID ) == 0 )
    {
        static constexpr char cms_type[] = UPNP_SERVICE_TYPE( "ConnectionManager" );
        if ( strcmp( action_name, "GetProtocolInfo" ) == 0 )
        {
            UpnpAddToActionResponse( &action_result, action_name, cms_type, "Source",
                                     "http-get:*:*:*" );
            UpnpAddToActionResponse( &action_result, action_name, cms_type, "Sink", "" );
            UpnpActionRequest_set_ActionResult( p_request, action_result );
        }
        else if ( strcmp( action_name, "GetCurrentConnectionIDs" ) == 0 )
        {
            UpnpAddToActionResponse( &action_result, action_name, cms_type, "ConnectionIDs", "" );
            UpnpActionRequest_set_ActionResult( p_request, action_result );
        }
    }

    else if ( strcmp( service_id, CDS_ID ) == 0 )
    {
        static constexpr char cds_type[] = UPNP_SERVICE_TYPE( "ContentDirectory" );

        if ( strcmp( action_name, "Browse" ) == 0 )
        {
            if ( !cds_browse( p_request, p_intf ) )
                msg_Err( p_intf, "server: failed to respond to browse action request" );
        }
        else if ( strcmp( action_name, "GetSearchCapabilities" ) == 0 )
        {
            UpnpAddToActionResponse( &action_result, action_name, cds_type, "SearchCaps", "" );
            UpnpActionRequest_set_ActionResult( p_request, action_result );
        }
        else if ( strcmp( action_name, "GetSortCapabilities" ) == 0 )
        {
            UpnpAddToActionResponse( &action_result, action_name, cds_type, "SortCaps", "" );
            UpnpActionRequest_set_ActionResult( p_request, action_result );
        }
        else if ( strcmp( action_name, "GetSystemUpdateID" ) == 0 )
        {
            char* psz_update_id;
            if ( asprintf( &psz_update_id, "%d", p_sys->upnp_update_id.load() ) == -1 )
                return;
            auto up_update_id = vlc::wrap_cptr( psz_update_id );
            UpnpAddToActionResponse( &action_result, action_name, cds_type, "Id", psz_update_id );
            UpnpActionRequest_set_ActionResult( p_request, action_result );
        }
    }
    else if ( strcmp( service_id, "urn:microsoft.com:serviceId:X_MS_MediaReceiverRegistrar" ) == 0 ) {
        // We need a mockup of this service to support Microsoft devices and clients.
        if ( strcmp( action_name, "IsAuthorized" ) == 0 )
        {
            UpnpAddToActionResponse( &action_result, action_name, CDS_ID, "Result", "1" );
            UpnpActionRequest_set_ActionResult( p_request, action_result );
        }
        else if ( strcmp( action_name, "IsValidated" ) == 0 )
        {
            UpnpAddToActionResponse( &action_result, action_name, CDS_ID, "Result", "1" );
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
        msg_Dbg( p_intf, "server: action request" );
        // We need to const_cast here because the upnp callback has to take a const void* for the
        // event data even if the sdk also expect us to modify it sometimes (in the case of a upnp
        // response for example)
        auto* rq =
            const_cast<UpnpActionRequest*>( static_cast<const UpnpActionRequest*>( p_event ) );
        handle_action_request( rq, p_intf );
    }
    break;
    case UPNP_CONTROL_GET_VAR_REQUEST:
        msg_Dbg( p_intf, "server: var request" );
        break;

    case UPNP_EVENT_SUBSCRIPTION_REQUEST:
        msg_Dbg( p_intf, "server: sub request" );
        break;

    default:
        msg_Err( p_intf, "server: unhandled event: %d", event_type );
        return UPNP_E_INVALID_ACTION;
    }

    return UPNP_E_SUCCESS;
}

// UPNP Callbacks

static int
getinfo_cb( const char* url, UpnpFileInfo* info, intf_thread_t* intf, FileHandler** fhandler )
{
    const char* user_agent = UpnpFileInfo_get_Os_cstr(info);
    if (user_agent == nullptr)
      return UPNP_E_BAD_REQUEST;

    msg_Dbg( intf, "GetInfo callback on: \"%s\" from: \"%s\"", url, user_agent );

    UpnpFileInfo_set_IsReadable( info, true );
    UpnpFileInfo_set_IsDirectory( info, false );

    auto* header_list = (UpnpListHead*)UpnpFileInfo_get_ExtraHeadersList(info);
    const auto & client_profile = intf->p_sys->profile_from_headers(header_list, user_agent);
    auto file_handler = parse_url( url, client_profile, *intf->p_sys );

    if ( file_handler == nullptr )
        return UPNP_E_FILE_NOT_FOUND;

    file_handler->get_info( *info );

    // Pass the filehandler ownership to the open callback to avoid reparsing the url
    *fhandler = file_handler.release();

    return UPNP_E_SUCCESS;
}

static UpnpWebFileHandle
open_cb( const char* url, enum UpnpOpenFileMode, intf_thread_t* intf, FileHandler* file_handler )
{
    msg_Dbg( intf, "Opening: %s", url );

    FileHandler* ret = file_handler;
    if ( !ret )
        ret = parse_url( url, intf->p_sys->default_profile(), *intf->p_sys ).release();

    if ( ret == nullptr )
        return nullptr;

    if ( !ret->open( intf ) )
    {
        msg_Err( intf, "Failed to open %s", url );
        delete ret;
        return nullptr;
    }
    return ret;
}

static int
read_cb( UpnpWebFileHandle fileHnd, uint8_t buf[], size_t buflen, intf_thread_t* intf, const void* )
{
    assert( fileHnd );

    const size_t bytes_read = static_cast<FileHandler*>( fileHnd )->read( buf, buflen );

    msg_Dbg( intf, "http read callback, %zub requested %zub returned", buflen, bytes_read );

    return bytes_read;
}

static int
seek_cb( UpnpWebFileHandle fileHnd, off_t offset, int origin, intf_thread_t* intf, const void* )
{
    assert( fileHnd );
    msg_Dbg( intf, "http seek callback offset: %ld origin: %d", offset, origin );

    const bool success = static_cast<FileHandler*>( fileHnd )->seek(
        static_cast<FileHandler::SeekType>( origin ), offset );
    return success == true ? 0 : -1;
}

static int close_cb( UpnpWebFileHandle fileHnd, intf_thread_t* intf, const void* )
{
    assert( fileHnd );
    msg_Dbg( intf, "http close callback" );
    delete static_cast<FileHandler*>( fileHnd );
    return 0;
}

static xml::Document make_server_identity(const char *uuid, const char *server_name) {
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

  xml::Element dlna_doc = ret.create_element("dlna:X_DLNADOC", ret.create_text_node("DMS-1.50"));
  dlna_doc.set_attribute("xmlns:dlna", "urn:schemas-dlna-org:device-1-0");

  xml::Element root = ret.create_element("root",
      ret.create_element("specVersion",
        ret.create_element("major", ret.create_text_node( "1")),
        ret.create_element("minor", ret.create_text_node( "0"))
      ),
      ret.create_element("device",
        std::move(dlna_doc),

        ret.create_element("deviceType", ret.create_text_node("urn:schemas-upnp-org:device:MediaServer:1")),
        ret.create_element("presentationUrl", ret.create_text_node(url.c_str())),
        ret.create_element("friendlyName", ret.create_text_node(server_name)),
        ret.create_element("manufacturer", ret.create_text_node("VideoLAN")),
        ret.create_element("manufacturerURL", ret.create_text_node("https://videolan.org")),
        ret.create_element("modelDescription", ret.create_text_node("VLC UPNP Media Server")),
        ret.create_element("modelName", ret.create_text_node("VLC")),
        ret.create_element("modelNumber", ret.create_text_node( PACKAGE_VERSION )),
        ret.create_element("modelURL", ret.create_text_node("https://videolan.org/vlc/")),
        ret.create_element("serialNumber", ret.create_text_node("1")),
        ret.create_element("UDN", ret.create_text_node(uuid_attr.c_str())),
        ret.create_element("iconList",
          icon_elem("/vlc.png", "32", "32"),
          icon_elem("/vlc512x512.png", "512", "512")
        ),
        ret.create_element("serviceList",
          service_elem("ConnectionManager"),
          service_elem("ContentDirectory"),

          ret.create_element( "service",
            ret.create_element( "serviceType", ret.create_text_node( "urn:microsoft.com:service:X_MS_MediaReceiverRegistrar:1" ) ),
            ret.create_element( "serviceId", ret.create_text_node( "urn:microsoft.com:serviceId:X_MS_MediaReceiverRegistrar" ) ),
            ret.create_element( "SCPDURL", ret.create_text_node( "/X_MS_MediaReceiverRegistrar.xml" ) ),
            ret.create_element( "controlURL", ret.create_text_node( "/X_MS_MediaReceiverRegistrar/Control" ) ),
            ret.create_element( "eventSubURL", ret.create_text_node( "/X_MS_MediaReceiverRegistrar/Event" ) )
          )
        )
      )
  );
  root.set_attribute("xmlns", "urn:schemas-upnp-org:device-1-0");

  ret.set_entry(std::move(root));
  return ret;
}

static bool init_upnp( intf_thread_t* intf )
{
    intf_sys_t* sys = intf->p_sys;

    addon_uuid_t uuid;
    vlc_rand_bytes( uuid, sizeof( uuid ) );
    sys->uuid = vlc::wrap_cptr( addons_uuid_to_psz( &uuid ), &free );

    int res = UpnpEnableWebserver( true );
    if ( res != UPNP_E_SUCCESS )
    {
        msg_Err( intf, "server: enabling webserver failed: %s", UpnpGetErrorMessage( res ) );
        return false;
    }

    msg_Info( intf, "upnp server enabled on %s:%d", UpnpGetServerIpAddress(), UpnpGetServerPort() );

    const auto str_rootdir = utils::get_root_dir();

    res = UpnpSetWebServerRootDir( str_rootdir.c_str() );
    msg_Dbg( intf, "webserver root dir set to: \"%s\"", str_rootdir.c_str() );
    if ( res != UPNP_E_SUCCESS )
    {
        msg_Err( intf, "server: setting webserver root dir failed: %s",
                 UpnpGetErrorMessage( res ) );
        UpnpEnableWebserver( false );
        return false;
    }

    const auto server_name =
        vlc::wrap_cptr( var_InheritString( intf, SERVER_PREFIX "name" ), &free );
    assert( server_name );
    const auto presentation_doc = make_server_identity( sys->uuid.get(), server_name.get() );
    const auto up_presentation_str = presentation_doc.to_wrapped_cstr();
    msg_Dbg( intf, "%s", up_presentation_str.get() );
    res = UpnpRegisterRootDevice2( UPNPREG_BUF_DESC, up_presentation_str.get(),
                                   strlen( up_presentation_str.get() ), 1, Callback, intf,
                                   &sys->p_device_handle );
    if ( res != UPNP_E_SUCCESS )
    {
        msg_Err( intf, "server: registration failed: %s", UpnpGetErrorMessage( res ) );
        UpnpEnableWebserver( false );
        return false;
    }

    sys->upnp_running.store( true );

    UpnpVirtualDir_set_GetInfoCallback( reinterpret_cast<VDCallback_GetInfo>( getinfo_cb ) );
    UpnpVirtualDir_set_OpenCallback( reinterpret_cast<VDCallback_Open>( open_cb ) );
    UpnpVirtualDir_set_ReadCallback( reinterpret_cast<VDCallback_Read>( read_cb ) );
    UpnpVirtualDir_set_SeekCallback( reinterpret_cast<VDCallback_Seek>( seek_cb ) );
    UpnpVirtualDir_set_CloseCallback( reinterpret_cast<VDCallback_Close>( close_cb ) );

    UpnpAddVirtualDir( "/media", intf, nullptr );
    UpnpAddVirtualDir( "/thumbnail", intf, nullptr );
    UpnpAddVirtualDir( "/subtitle", intf, nullptr );

    res = UpnpSendAdvertisement( sys->p_device_handle, 1800 );
    if ( res != UPNP_E_SUCCESS )
    {
        msg_Dbg( intf, "server: advertisement failed: %s", UpnpGetErrorMessage( res ) );
        sys->close_server();
        return false;
    }

    sys->upnp_update_id = 0;

    return true;
}

// Spawn a warning privacy dialog and then init the UPNP server.
// Must be called in a separate thread to avoid blocking the Open callback.
static void* init_upnp_with_dialog( void* opaque )
{
    auto* intf = static_cast<intf_thread_t*>( opaque );
    intf_sys_t* sys = intf->p_sys;

    vlc_interrupt_set( sys->interrupt.get() );

    static constexpr auto title = N_( "UPNP Server startup" );
    static constexpr auto warning =
        N_( "VLC is about to start the UPNP server module, this protocol will expose your indexed "
            "media on the current network. Please make sure you are on a trusted network before "
            "pressing continue." );
    const int dialog =
        vlc_dialog_wait_question( VLC_OBJECT( intf ), VLC_DIALOG_QUESTION_WARNING, N_( "Cancel" ),
                                  N_( "Continue" ), nullptr, title, warning );

    vlc_interrupt_forward_stop( sys->interrupt_data );

    if ( dialog < 0 )
    {
        msg_Err(
            intf,
            N_( "The UPNP server exposes your indexed media on the current network which is risked "
                "if you are not on a trusted network. If you are sure you are on a secure network, "
                "restart with the option `--" SERVER_PREFIX
                "accept-risk\" to enable the server." ) );
        return nullptr;
    }
    else if ( dialog != 1 )
    {
        msg_Info( intf, N_( "User canceled the UPNP server startup by canceling the dialog." ) );
        return nullptr;
    }

    init_upnp( intf );
    return nullptr;
}

namespace Server
{

int open( vlc_object_t* p_this )
{
    intf_thread_t* p_intf = container_of(p_this, intf_thread_t, obj);
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
    auto* ml_callback_handle =
        vlc_ml_event_register_callback( p_sys->p_ml, medialibrary_event_callback, p_intf );
    vlc_medialibrary_t* p_ml = p_sys->p_ml;
    p_sys->ml_callback_handle = { ml_callback_handle, std::bind( vlc_ml_event_unregister_callback,
                                                                 p_ml, std::placeholders::_1 ) };

    p_sys->upnp = vlc::wrap_cptr( UpnpInstanceWrapper::get( p_this ),
                                  []( UpnpInstanceWrapper* p_upnp ) { p_upnp->release(); } );

    p_sys->obj_hierarchy = cds::init_hierarchy( *p_ml );
    p_sys->accept_risk = var_InheritBool( p_this, SERVER_PREFIX "accept-risk" );

    // In the case the accept risk option is passed, we know the user does not need privacy warnings
    // and basically should know what he's doing. Calling the module with this option initiate the
    // UPNP server in the `Open` thread as the operation isn't blocking anymore.
    if ( p_sys->accept_risk == true )
    {
        if ( init_upnp( p_intf ) == false )
        {
            delete p_sys;
            return VLC_EGENERIC;
        }
        return VLC_SUCCESS;
    }

    // In the case where accept-risk isn't passed, initialization is done in a separate
    // interruptible thread that will wait for user confirmation.
    vlc_interrupt_forward_start( p_sys->interrupt.get(), p_sys->interrupt_data );
    const int status =
        vlc_clone( &p_sys->init_thread, &init_upnp_with_dialog, p_intf, VLC_THREAD_PRIORITY_LOW );
    if ( status != VLC_SUCCESS )
    {
        delete p_sys;
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

void close( vlc_object_t* p_this )
{
    intf_thread_t* intf = container_of(p_this, intf_thread_t, obj);
    intf_sys_t* sys = intf->p_sys;
    if ( sys->accept_risk == false )
    {
        vlc_interrupt_kill( sys->interrupt.get() );
        vlc_join( sys->init_thread, nullptr );
    }

    sys->close_server();
    delete sys;
}
} // namespace Server
