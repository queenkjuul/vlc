#pragma once

#include "Container.hpp"

#include "Item.hpp"

#include <vlc_media_library.h>

namespace cds
{

namespace
{
inline void dump_mlobject_metadata( xml::Element& dest,
                                         const vlc_ml_media_t& media,
                                         const TranscodeProfileList& profiles )
{
    Item::dump_mlobject_metadata( dest, media, profiles );
}

inline void dump_mlobject_metadata( xml::Element& dest,
                                         const vlc_ml_playlist_t& playlist,
                                         const TranscodeProfileList& )
{
    xml::Document& doc = dest.owner;

    //TODO this is not exposed yet in the medialibrary
    //dest.set_attribute( "childCount", std::to_string( playlist.i_nb_tracks ).c_str() );

    dest.add_children(
        doc.create_element( "upnp:class",
                            doc.create_text_node( "object.container.playlistContainer" ) ),
        doc.create_element( "dc:title", doc.create_text_node( playlist.psz_name ) ) );
}

inline void dump_mlobject_metadata( xml::Element& dest,
                                         const vlc_ml_show_t& show,
                                         const TranscodeProfileList& )
{
    xml::Document& doc = dest.owner;

    //TODO this is not exposed yet in the medialibrary
    //dest.set_attribute( "childCount", std::to_string( playlist.i_nb_tracks ).c_str() );

    const std::string nb_episodes = std::to_string( show.i_nb_episodes );
    dest.set_attribute( "childCount", nb_episodes.c_str() );
    dest.add_children(
        doc.create_element( "upnp:class",
                            doc.create_text_node( "object.container.playlistContainer" ) ),
        doc.create_element( "dc:title", doc.create_text_node( show.psz_name ) ),
        doc.create_element( "upnp:episodeCount", doc.create_text_node( nb_episodes.c_str() ) ) );
}

inline void dump_mlobject_metadata( xml::Element& dest,
                                         const vlc_ml_album_t& album,
                                         const TranscodeProfileList& )
{
    xml::Document& doc = dest.owner;

    dest.set_attribute( "childCount", std::to_string( album.i_nb_tracks ).c_str() );

    dest.add_children(
        doc.create_element( "upnp:artist", doc.create_text_node( album.psz_artist ) ),
        doc.create_element( "upnp:class",
                            doc.create_text_node( "object.container.album.musicAlbum" ) ),
        doc.create_element( "dc:title", doc.create_text_node( album.psz_title ) ),
        doc.create_element( "dc:description", doc.create_text_node( album.psz_summary ) ) );
}

inline void dump_mlobject_metadata( xml::Element& dest,
                                         const vlc_ml_artist_t& artist,
                                         const TranscodeProfileList& )
{
    xml::Document& doc = dest.owner;

    dest.set_attribute( "childCount", std::to_string( artist.i_nb_album ).c_str() );

    dest.add_children(
        doc.create_element( "upnp:class",
                            doc.create_text_node( "object.container.person.musicArtist" ) ),
        doc.create_element( "dc:title", doc.create_text_node( artist.psz_name ) ) );
}
} // namespace
template <typename MLHelper>
class MLContainer : public Container
{
    private:
    vlc_medialibrary_t& ml;
    const TranscodeProfileList& transcode_profiles;
    const Object& child;

    public:
    MLContainer( int64_t id,
                 int64_t parent_id,
                 const char* name,
                 vlc_medialibrary_t& ml,
                 const TranscodeProfileList& profiles,
                 const Object& child ) noexcept
        : Container( id, parent_id, name, MLHelper::count( ml, opt::nullopt ) )
        , ml( ml )
        , transcode_profiles( profiles )
        , child( child )
    {
    }

    void dump_metadata( xml::Element& dest, const Object::ExtraId& extra ) const final
    {
        if ( extra.has_value() )
        {
            const auto& ml_object = MLHelper::get( ml, extra.value().ml_id );
            dump_mlobject_metadata( dest, *ml_object.get(), transcode_profiles );
        }
        else
        {
            Container::dump_metadata( dest, opt::nullopt );
        }
    }

    BrowseStats browse_direct_children( xml::Element& dest,
                                        const BrowseParams params,
                                        const Object::ExtraId& extra ) const final
    {
        const vlc_ml_query_params_t query_params = { nullptr, params.requested, params.offset,
                                                     VLC_ML_SORTING_DEFAULT, true };
        opt::Option<int64_t> ml_id;
        if ( extra.has_value() )
            ml_id = static_cast<int64_t>( extra->ml_id );
        const auto list = MLHelper::list( ml, &query_params, ml_id );

        xml::Document& doc = dest.owner;
        for ( unsigned i = 0; i < list->i_nb_items; ++i )
        {
            const auto& item = list->p_items[i];

            auto elem = child.create_object_element(
                doc, ExtraIdData{ item.i_id, get_dynamic_id( extra ) } );
            dump_mlobject_metadata( elem, item, transcode_profiles );
            dest.add_child( std::move( elem ) );
        }
        return { list->i_nb_items, MLHelper::count( ml, ml_id ) };
    }
};
} // namespace cds
