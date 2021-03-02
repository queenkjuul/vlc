#pragma once

#include "Container.hpp"

#include <vlc_media_library.h>
#include <vlc_cxx_helpers.hpp>
#include <vlc_url.h>

#include "../utils.hpp"
#include "Item.hpp"

namespace cds
{

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
            dump_mlobject_metadata( dest, *ml_object.get() );
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
            dump_mlobject_metadata( elem, item );
            dest.add_child( std::move( elem ) );
        }
        return { list->i_nb_items, MLHelper::count( ml, ml_id ) };
    }

    private:
    void dump_mlobject_metadata( xml::Element& dest, const vlc_ml_media_t& media ) const
    {
        Item::dump_mlobject_metadata( dest, media, ml, transcode_profiles );
    }

    void dump_mlobject_metadata( xml::Element& dest, const vlc_ml_playlist_t& playlist ) const
    {
        xml::Document& doc = dest.owner;

        // TODO this is not exposed yet in the medialibrary
        // dest.set_attribute( "childCount", std::to_string( playlist.i_nb_tracks ).c_str() );

        dest.add_children(
            doc.create_element( "upnp:class",
                                doc.create_text_node( "object.container.playlistContainer" ) ),
            doc.create_element( "dc:title", doc.create_text_node( playlist.psz_name ) ) );
    }

    void dump_mlobject_metadata( xml::Element& dest, const vlc_ml_album_t& album ) const
    {
        xml::Document& doc = dest.owner;

        dest.set_attribute( "childCount", std::to_string( album.i_nb_tracks ).c_str() );

        const auto album_thumbnail_url = utils::album_thumbnail_url( album );
        dest.add_children(
            doc.create_element( "upnp:artist", doc.create_text_node( album.psz_artist ) ),
            doc.create_element( "upnp:class",
                                doc.create_text_node( "object.container.album.musicAlbum" ) ),
            doc.create_element( "dc:title", doc.create_text_node( album.psz_title ) ),
            doc.create_element( "dc:description", doc.create_text_node( album.psz_summary ) ),
            doc.create_element( "upnp:albumArtURI",
                                doc.create_text_node( album_thumbnail_url.c_str() ) ) );
    }

    void dump_mlobject_metadata( xml::Element& dest, const vlc_ml_artist_t& artist ) const
    {
        xml::Document& doc = dest.owner;

        dest.set_attribute( "childCount", std::to_string( artist.i_nb_album ).c_str() );

        dest.add_children(
            doc.create_element( "upnp:class",
                                doc.create_text_node( "object.container.person.musicArtist" ) ),
            doc.create_element( "dc:title", doc.create_text_node( artist.psz_name ) ) );
    }

    void dump_mlobject_metadata( xml::Element& dest, const vlc_ml_genre_t& genre ) const
    {
        xml::Document& doc = dest.owner;

        dest.set_attribute( "childCount", std::to_string( genre.i_nb_tracks ).c_str() );

        dest.add_children(
            doc.create_element( "upnp:class",
                                doc.create_text_node( "object.container.genre.musicGenre" ) ),
            doc.create_element( "dc:title", doc.create_text_node( genre.psz_name ) ) );
    }

    void dump_mlobject_metadata( xml::Element& dest, const vlc_ml_show_t& show ) const
    {
        xml::Document& doc = dest.owner;

        const std::string nb_episodes = std::to_string( show.i_nb_episodes );
        dest.set_attribute( "childCount", nb_episodes.c_str() );
        dest.add_children(
            doc.create_element( "upnp:class",
                                doc.create_text_node( "object.container.playlistContainer" ) ),
            doc.create_element( "dc:title", doc.create_text_node( show.psz_name ) ),
            doc.create_element( "upnp:episodeCount",
                                doc.create_text_node( nb_episodes.c_str() ) ) );
    }
    void dump_mlobject_metadata( xml::Element& dest, const vlc_ml_folder_t& folder ) const
    {
        xml::Document& doc = dest.owner;

        assert(!folder.b_banned);

        const auto path = vlc::wrap_cptr( vlc_uri2path( folder.psz_mrl ), &free );
        dest.add_children(
            doc.create_element( "upnp:class", doc.create_text_node( "object.container" ) ),
            doc.create_element( "dc:title", doc.create_text_node( path.get() ) ) );
    }
};
} // namespace cds
