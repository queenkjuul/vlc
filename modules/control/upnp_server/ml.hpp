#include "Common.hpp"

#include "Option.hpp"

#include <functional>
#include <memory>
#include <type_traits>

namespace ml
{

namespace
{
template <typename T>
void ml_object_release( T* obj ) noexcept
{
    vlc_ml_release( obj );
}
} // namespace

template <typename MLObject, vlc_ml_get_queries GetQuery>
struct Object
{
    using Ptr = std::unique_ptr<MLObject, std::function<void( MLObject* )>>;
    static Ptr get( vlc_medialibrary_t& ml, const int64_t id ) noexcept
    {
        MLObject* obj = static_cast<MLObject*>( vlc_ml_get( &ml, GetQuery, id ) );
        return Ptr{ obj, &ml_object_release<MLObject> };
    }
};

using Media = Object<vlc_ml_media_t, VLC_ML_GET_MEDIA>;
using Album = Object<vlc_ml_album_t, VLC_ML_GET_ALBUM>;
using Playlist = Object<vlc_ml_playlist_t, VLC_ML_GET_PLAYLIST>;
using Show = Object<vlc_ml_show_t, VLC_ML_GET_SHOW>;
using Artist = Object<vlc_ml_artist_t, VLC_ML_GET_ARTIST>;

template <typename ListType,
          vlc_ml_list_queries ListQuery,
          vlc_ml_list_queries CountQuery,
          typename Object>
struct List : Object
{
  using Entry = typename std::decay<decltype(ListType().p_items[0])>::type;
    static size_t count( vlc_medialibrary_t& ml, opt::Option<int64_t> id ) noexcept
    {
        size_t res;
        int status;

        if ( id.has_value() )
            status = vlc_ml_list( &ml, CountQuery, nullptr, id.value(), &res );
        else if ( CountQuery == VLC_ML_COUNT_ARTISTS )
            status = vlc_ml_list( &ml, CountQuery, nullptr, (int)false, &res );
        else
            status = vlc_ml_list( &ml, CountQuery, nullptr, &res );
        return status == VLC_SUCCESS ? res : 0;
    }

    using Ptr = std::unique_ptr<ListType, std::function<void( ListType* )>>;
    static Ptr list( vlc_medialibrary_t& ml,
                     const vlc_ml_query_params_t* params,
                     const opt::Option<int64_t> id ) noexcept
    {
        ListType* res;
        int status;

        if ( id.has_value() )
            status = vlc_ml_list( &ml, ListQuery, params, id.value(), &res );
        else if ( ListQuery == VLC_ML_LIST_ARTISTS )
            status = vlc_ml_list( &ml, ListQuery, params, (int)false, &res );
        else
            status = vlc_ml_list( &ml, ListQuery, params, &res );
        return { status == VLC_SUCCESS ? res : nullptr, &ml_object_release<ListType> };
    }
};

using AllAudioList = List<vlc_ml_media_list_t, VLC_ML_LIST_AUDIOS, VLC_ML_COUNT_AUDIOS, Media>;
using AllVideoList = List<vlc_ml_media_list_t, VLC_ML_LIST_VIDEOS, VLC_ML_COUNT_VIDEOS, Media>;
using AllAlbumList = List<vlc_ml_album_list_t, VLC_ML_LIST_ALBUMS, VLC_ML_COUNT_ALBUMS, Media>;
using ArtistAlbumList =
    List<vlc_ml_album_list_t, VLC_ML_LIST_ARTIST_ALBUMS, VLC_ML_COUNT_ARTIST_ALBUMS, Album>;
using ArtistTracksList =
    List<vlc_ml_media_list_t, VLC_ML_LIST_ARTIST_TRACKS, VLC_ML_COUNT_ARTIST_TRACKS, Media>;
using AllArtistsList = List<vlc_ml_artist_list_t, VLC_ML_LIST_ARTISTS, VLC_ML_COUNT_ARTISTS, Media>;
using AlbumTracksList =
    List<vlc_ml_media_list_t, VLC_ML_LIST_ALBUM_TRACKS, VLC_ML_COUNT_ALBUM_TRACKS, Album>;
using PlaylistsList =
    List<vlc_ml_playlist_list_t, VLC_ML_LIST_PLAYLISTS, VLC_ML_COUNT_PLAYLISTS, Playlist>;
using PlaylistMediaList =
    List<vlc_ml_media_list_t, VLC_ML_LIST_PLAYLIST_MEDIA, VLC_ML_COUNT_PLAYLIST_MEDIA, Media>;
using ShowsList =
    List<vlc_ml_show_list_t, VLC_ML_LIST_SHOWS, VLC_ML_COUNT_SHOW_EPISODES, Show>;
using ShowsMediaList =
    List<vlc_ml_media_list_t, VLC_ML_LIST_SHOW_EPISODES, VLC_ML_COUNT_SHOW_EPISODES, Media>;

} // namespace ml
