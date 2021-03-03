/*****************************************************************************
 * upnp_server.h : UPnP server module header
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

#pragma once

#include "Common.hpp"
#include "cds/cds.hpp"

#include "../../services_discovery/upnp-wrapper.hpp"

#include <vlc_block.h>
#include <vlc_stream.h>
#include <vlc_threads.h>

#include <upnp/ExtraHeaders.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include <unordered_map>

#define CDS_ID "urn:upnp-org:serviceId:ContentDirectory"
#define CMS_ID "urn:upnp-org:serviceId:ConnectionManager"

#define UPNP_SERVICE_TYPE(service) "urn:schemas-upnp-org:service:" service ":1"

#define CDS_OBJECT_ID "id"
#define CDS_OBJECT_PARENT_ID "parentID"
#define CDS_OBJECT_TITLE "dc:title"
#define CDS_OBJECT_CLASS "upnp:class"
#define CDS_OBJECT_RESOURCE "res"
#define CDS_OBJECT_PROTOCOL_INFO "protocolInfo"
#define CDS_OBJECT_RESTRICTED "restricted"
#define CDS_OBJECT_SEARCHABLE "searchable"
#define CDS_OBJECT_CHILD_COUNT "childCount"

#define CDS_OBJECT_ARTIST "upnp:artist"
#define CDS_OBJECT_ALBUM "upnp:album"
#define CDS_OBJECT_GENRE "upnp:genre"
#define CDS_OBJECT_DESCRIPTION "dc:description"
#define CDS_OBJECT_DATE "dc:date"
#define CDS_OBJECT_ARTWORK "upnp:albumArtURI"
#define CDS_OBJECT_DISCOGRAPHY "upnp:artistDiscographyURI"

#define CDS_ITEM_AUDIO "object.item.audioItem"
#define CDS_ITEM_VIDEO "object.item.videoItem"
#define CDS_ITEM_PLAYLIST "object.item.playlistItem"

#define CDS_ITEM_TRACK "object.item.audioItem.musicTrack"
#define CDS_ITEM_MOVIE "object.item.videoItem.movie"

#define CDS_CONTAINER_ALBUM "object.container.album.musicAlbum"
#define CDS_CONTAINER_GENRE "object.container.genre.musicGenre"
#define CDS_CONTAINER_ARTIST "object.container.person.musicArtist"
#define CDS_CONTAINER_PLAYLIST "object.container.playlistContainer"

#define CDS_ITEM "object.item"
#define CDS_CONTAINER "object.container"

enum fixed_containers
{
    root,
    musics,
    videos,
    playlists,
    all_tracks,
    albums,
    artists,
    genres,
    all_videos,
    shows,
    nb_containers
};

enum cds_object_type
{
    fixed_container,
    media,
    album,
    artist,
    genre,
    show,
    playlist,
    by_track,
    by_album
};

typedef struct fixed_container_t
{
    const char* psz_name;

    std::vector<int> children_id;
} fixed_container_t;

typedef struct intf_sys_t
{
    intf_sys_t()
        : up_ml_callback_handle( nullptr,
                                 [=]( vlc_ml_event_callback_t* p_ch ) {
                                     vlc_ml_event_unregister_callback( p_ml, p_ch );
                                 } )
        , up_uuid( nullptr, &free )
        , up_upnp( nullptr, []( UpnpInstanceWrapper* p_upnp ) { p_upnp->release(); } )
    {
    }

    vlc_medialibrary_t* p_ml;
    vlc_ml_event_callback_t* p_ml_callback_handle;

    std::unique_ptr<vlc_ml_event_callback_t, std::function<void( vlc_ml_event_callback_t* )>>
        up_ml_callback_handle;

    std::unique_ptr<char, decltype( &free )> up_uuid;

    UpnpDevice_Handle p_device_handle;

    std::unique_ptr<UpnpInstanceWrapper, std::function<void( UpnpInstanceWrapper* )>> up_upnp;

    std::atomic<unsigned int> i_update_id;

    std::vector<std::unique_ptr<cds::Object>> hierarchy;

    static const std::unordered_map<std::string, ClientProfile> CLIENT_PROFILES;
    static const ClientProfile DEFAULT_PROFILES;

    static const ClientProfile &profile_from_headers(UpnpListHead*, const char *user_agent) noexcept;
} intf_sys_t;

struct Fifo
{
    static const char VAR_NAME[];
    // Doesn't need to be atomic, thread-safety is already ensured by the block_fifo cond_var
    bool eof = false;

    std::unique_ptr<block_fifo_t, decltype( &block_FifoRelease )> queue = { block_FifoNew(),
                                                                            &block_FifoRelease };

    size_t read( uint8_t buf[], size_t buflen ) noexcept;
};

namespace Server
{
int open( vlc_object_t* p_this );
void close( vlc_object_t* p_this );

namespace AccessOut
{
int open( vlc_object_t* );
void close( vlc_object_t* );
} // namespace AccessOut

namespace StreamOutProxy
{
int open( vlc_object_t* p_this );
void close( vlc_object_t* p_this );
} // namespace StreamOutProxy
} // namespace Server
