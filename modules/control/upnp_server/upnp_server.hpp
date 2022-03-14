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

#include <UpnpExtraHeaders.h>
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <vlc_common.h>

#include <vlc_block.h>
#include <vlc_media_library.h>

#include "../../services_discovery/upnp-wrapper.hpp"
#include "Profiles.hpp"
#include "cds/cds.hpp"

#define SERVER_PREFIX "upnp-server-"

#define SERVER_DEFAULT_NAME N_("VLC Media Server")
#define SERVER_NAME_DESC N_("Upnp server name")
#define SERVER_NAME_LONGTEXT N_("The client exposed upnp server name")

#define SERVER_FORCE_TRANSCODE_TEXT N_("Force media transcoding")
#define SERVER_FORCE_TRANSCODE_LONGTEXT N_("Only expose transcoded media profiles to clients")

struct intf_sys_t
{

    vlc_medialibrary_t *p_ml;
    std::unique_ptr<vlc_ml_event_callback_t, std::function<void(vlc_ml_event_callback_t *)>>
        ml_callback_handle;

    std::unique_ptr<char, decltype(&free)> uuid;

    UpnpDevice_Handle p_device_handle;
    std::unique_ptr<UpnpInstanceWrapper, std::function<void(UpnpInstanceWrapper *)>> upnp;

    // This integer is atomically incremented at each medialib modification. It will be sent in each
    // response. If the client notice that the update id has been incremented since the last
    // request, he knows that the server state has changed and hence can refetch the exposed
    // hierarchy.
    std::atomic<unsigned int> upnp_update_id;

    std::vector<std::unique_ptr<cds::Object>> obj_hierarchy;

    intf_sys_t() :
        ml_callback_handle(
            nullptr,
            [&](vlc_ml_event_callback_t *p_ch) { vlc_ml_event_unregister_callback(p_ml, p_ch); }),
        uuid(nullptr, &free),
        upnp(nullptr, [](UpnpInstanceWrapper *p_upnp) { p_upnp->release(); })
    {}

    // Client profiles describe the way medias should be exposed to certains clients.
    // While most upnp/dlna clients are easy and flexible, some of them needs custom transcoding
    // profiles due to their very strict implementation and/or poor codec coverage.
    static const ClientProfile &profile_from_headers(UpnpListHead *, const char *user_agent);
    static const ClientProfile &default_profile() noexcept;

    // In some case we might disable the upnp server before the module stops.
    // This boolean ensure we don't try to stop the server twice.
    std::atomic_bool upnp_running;

    void close_server()
    {
        if (!upnp_running.load())
            return;
        UpnpUnRegisterRootDevice(p_device_handle);
        UpnpEnableWebserver(false);
        upnp_running.store(false);
    }
    bool extra_verbose;
};

/// Transcoded media data blocks are held into that FIFO waiting the http upnp callbacks
/// to send them. The AccessOut fill the fifo while the http servers empty it.
struct TranscodeFifo
{
    static const char VAR_NAME[];
    bool eof = false;

    std::unique_ptr<block_fifo_t, decltype(&block_FifoRelease)> queue = {block_FifoNew(),
                                                                         &block_FifoRelease};

    size_t read(uint8_t buf[], size_t buflen) noexcept;
};

namespace Server
{
int open(vlc_object_t *p_this);
void close(vlc_object_t *p_this);

/// We use this AccessOut to store the output of the upnp transcode pipeline into a block fifo.
/// This module simply copy arriving blocks into a fifo and pace the muxer when the fifo is full.
/// The fifo is emptied by the libupnp http callbacks thread.
namespace AccessOut
{
int open(vlc_object_t *);
void close(vlc_object_t *);
} // namespace AccessOut

/// This small sout is the entry point of the transcode chain when the client
/// request the media with a specific starting time (other than 0).
/// This simple stream makes sure that nothing is sent before the
/// flush operation right after the player seek.
/// This is a hacky way to ensure the client receive no discontinuity caused
/// by seeking at the beginning of the stream.
namespace StreamOutProxy
{
int open(vlc_object_t *p_this);
} // namespace StreamOutProxy
} // namespace Server

#endif /* UPNP_SERVER_HPP */
