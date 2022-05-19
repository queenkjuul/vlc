/*****************************************************************************
 * ml.hpp : C++ media library APi wrapper
 *****************************************************************************
 * Copyright © 2021 VLC authors and VideoLAN
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
#ifndef ML_HPP
#define ML_HPP

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_media_library.h>

#include "Option.hpp"

#include <functional>
#include <memory>
#include <type_traits>

namespace ml
{

template <typename MLObject, vlc_ml_get_queries GetQuery>
struct Object
{
    using Ptr = std::unique_ptr<MLObject, std::function<void( MLObject* )>>;
    static Ptr get( vlc_medialibrary_t& ml, const int64_t id ) noexcept
    {
        MLObject* obj = static_cast<MLObject*>( vlc_ml_get( &ml, GetQuery, id ) );
        return Ptr{ obj, static_cast<void ( * )( MLObject* )>( &vlc_ml_release ) };
    }
};

using Media = Object<vlc_ml_media_t, VLC_ML_GET_MEDIA>;
using Album = Object<vlc_ml_album_t, VLC_ML_GET_ALBUM>;
using Playlist = Object<vlc_ml_playlist_t, VLC_ML_GET_PLAYLIST>;
using Show = Object<vlc_ml_show_t, VLC_ML_GET_SHOW>;
using Artist = Object<vlc_ml_artist_t, VLC_ML_GET_ARTIST>;
using Genre = Object<vlc_ml_artist_t, VLC_ML_GET_GENRE>;
using Folder = Object<vlc_ml_folder_t, VLC_ML_GET_FOLDER>;

template <typename ListType,
          vlc_ml_list_queries ListQuery,
          vlc_ml_list_queries CountQuery,
          typename Object>
struct List : Object
{
    static size_t count( vlc_medialibrary_t& ml, opt::Option<int64_t> id ) noexcept
    {
        size_t res;
        int status;

        const vlc_ml_query_params_t params = { nullptr, 0, 0, VLC_ML_SORTING_DEFAULT, false, true };

        if ( id.has_value() )
            status = vlc_ml_list( &ml, CountQuery, &params, id.value(), &res );
        else if ( CountQuery == VLC_ML_COUNT_ARTISTS )
            status = vlc_ml_list( &ml, CountQuery, &params, (int)false, &res );
        else if ( CountQuery == VLC_ML_COUNT_ENTRY_POINTS )
            status = vlc_ml_list( &ml, CountQuery, &params, (int)false, &res );
        else
            status = vlc_ml_list( &ml, CountQuery, &params, &res );
        return status == VLC_SUCCESS ? res : 0;
    }

    using Ptr = std::unique_ptr<ListType, std::function<void( ListType* )>>;
    static Ptr list( vlc_medialibrary_t& ml,
                     const vlc_ml_query_params_t* params,
                     const opt::Option<int64_t> id ) noexcept
    {
        ListType* res;
        int status;

        assert( params->b_public_only );

        if ( id.has_value() )
            status = vlc_ml_list( &ml, ListQuery, params, id.value(), &res );
        else if ( ListQuery == VLC_ML_LIST_ARTISTS )
            status = vlc_ml_list( &ml, ListQuery, params, (int)false, &res );
        else if ( ListQuery == VLC_ML_LIST_ENTRY_POINTS )
            status = vlc_ml_list( &ml, ListQuery, params, (int)false, &res );
        else
            status = vlc_ml_list( &ml, ListQuery, params, &res );
        return { status == VLC_SUCCESS ? res : nullptr,
                 static_cast<void ( * )( ListType* )>( &vlc_ml_release ) };
    }
};

using AllAudio = List<vlc_ml_media_list_t, VLC_ML_LIST_AUDIOS, VLC_ML_COUNT_AUDIOS, Media>;
using AllVideos = List<vlc_ml_media_list_t, VLC_ML_LIST_VIDEOS, VLC_ML_COUNT_VIDEOS, Media>;
using AllAlbums = List<vlc_ml_album_list_t, VLC_ML_LIST_ALBUMS, VLC_ML_COUNT_ALBUMS, Media>;
using AllEntryPoints = List<vlc_ml_folder_list_t, VLC_ML_LIST_ENTRY_POINTS, VLC_ML_COUNT_ENTRY_POINTS, Folder>;

using AllArtistsList = List<vlc_ml_artist_list_t, VLC_ML_LIST_ARTISTS, VLC_ML_COUNT_ARTISTS, Media>;
using ArtistAlbumList =
    List<vlc_ml_album_list_t, VLC_ML_LIST_ARTIST_ALBUMS, VLC_ML_COUNT_ARTIST_ALBUMS, Album>;
using ArtistTracksList =
    List<vlc_ml_media_list_t, VLC_ML_LIST_ARTIST_TRACKS, VLC_ML_COUNT_ARTIST_TRACKS, Media>;
using AlbumTracksList =
    List<vlc_ml_media_list_t, VLC_ML_LIST_ALBUM_TRACKS, VLC_ML_COUNT_ALBUM_TRACKS, Album>;

using AllGenresList = List<vlc_ml_genre_list_t, VLC_ML_LIST_GENRES, VLC_ML_COUNT_GENRES, Media>;
using GenreAlbumList =
    List<vlc_ml_album_list_t, VLC_ML_LIST_GENRE_ALBUMS, VLC_ML_COUNT_GENRE_ALBUMS, Album>;
using GenreTracksList =
    List<vlc_ml_media_list_t, VLC_ML_LIST_GENRE_TRACKS, VLC_ML_COUNT_GENRE_TRACKS, Media>;

using PlaylistsList =
    List<vlc_ml_playlist_list_t, VLC_ML_LIST_PLAYLISTS, VLC_ML_COUNT_PLAYLISTS, Playlist>;
using PlaylistMediaList =
    List<vlc_ml_media_list_t, VLC_ML_LIST_PLAYLIST_MEDIA, VLC_ML_COUNT_PLAYLIST_MEDIA, Media>;

using ShowsList = List<vlc_ml_show_list_t, VLC_ML_LIST_SHOWS, VLC_ML_COUNT_SHOW_EPISODES, Show>;
using ShowsMediaList =
    List<vlc_ml_media_list_t, VLC_ML_LIST_SHOW_EPISODES, VLC_ML_COUNT_SHOW_EPISODES, Media>;

using MediaFolderList = List<vlc_ml_media_list_t, VLC_ML_LIST_FOLDER_MEDIA, VLC_ML_COUNT_FOLDER_MEDIA, Media>;

} // namespace ml

#endif /* ML_HPP */
