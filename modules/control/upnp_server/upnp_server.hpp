/*****************************************************************************
 * upnp_server.hpp : UPnP server module header
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
#ifndef UPNP_SERVER_HPP
#define UPNP_SERVER_HPP

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_media_library.h>

#include "cds/cds.hpp"

#include "../../services_discovery/upnp-wrapper.hpp"

#include <vlc_block.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include <unordered_map>

#define SERVER_PREFIX "upnp-server-"

#define SERVER_DEFAULT_NAME N_("VLC Media Server")
#define SERVER_NAME_DESC N_("Upnp server name")
#define SERVER_NAME_LONGTEXT N_("The client exposed upnp server name")


struct intf_sys_t
{

    vlc_medialibrary_t* p_ml;
    std::unique_ptr<vlc_ml_event_callback_t, std::function<void( vlc_ml_event_callback_t* )>>
        ml_callback_handle;

    std::unique_ptr<char, decltype( &free )> uuid;

    UpnpDevice_Handle p_device_handle;
    std::unique_ptr<UpnpInstanceWrapper, std::function<void( UpnpInstanceWrapper* )>> upnp;

    // This integer is atomically incremented at each medialib modification. It will be sent in each
    // response. If the client notice that the update id has been incremented since the last
    // request, he knows that the server state has changed and hence can refetch the exposed
    // hierarchy.
    std::atomic<unsigned int> upnp_update_id;

    std::vector<std::unique_ptr<cds::Object>> obj_hierarchy;

    intf_sys_t()
        : ml_callback_handle( nullptr,
                              [&]( vlc_ml_event_callback_t* p_ch )
                              { vlc_ml_event_unregister_callback( p_ml, p_ch ); } )
        , uuid( nullptr, &free )
        , upnp( nullptr, []( UpnpInstanceWrapper* p_upnp ) { p_upnp->release(); } )
    {
    }

    // In some case we might disable the upnp server before the module stops.
    // This boolean ensure we don't try to stop the server twice.
    std::atomic_bool upnp_running;
    void close_server()
    {
        if ( !upnp_running.load() )
            return;
        UpnpUnRegisterRootDevice( p_device_handle );
        UpnpEnableWebserver( false );
        upnp_running.store( false );
    }
};

namespace Server
{
int open( vlc_object_t* p_this );
void close( vlc_object_t* p_this );
} // namespace Server

#endif /* UPNP_SERVER_HPP */
