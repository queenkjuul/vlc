/*****************************************************************************
 * MLContainer.hpp : CDS MediaLibrary container implementation
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
#pragma once
#ifndef MLCONTAINER_HPP
#define MLCONTAINER_HPP

#include "Container.hpp"

#include <vlc_cxx_helpers.hpp>
#include <vlc_media_library.h>
#include <vlc_url.h>

#include "Item.hpp"

namespace cds
{

/// MLContainer is a dynamic object, it must have a ml id in its extra id.
/// MLContainer is a very versatile Container that basically list all the medialibrary objects such
/// as Albums, Playlists, etc.
/// MLHelpers can be found in "../ml.hpp"
template <typename MLHelper>
class MLContainer : public Container
{
    private:
    vlc_medialibrary_t& ml;
    /// We take another dynamic object as member, this will be the dynamic child of the
    /// MLContainer, for example a MLContainer representing an album will have an Item
    /// ("./Item.hpp") as child
    const Object& child;

    public:
    MLContainer( int64_t id,
                 int64_t parent_id,
                 const char* name,
                 vlc_medialibrary_t& ml,
                 const Object& child ) noexcept
        : Container( id, parent_id, name, MLHelper::count( ml, opt::nullopt ) )
        , ml( ml )
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
        Item::dump_mlobject_metadata( dest, media, ml );
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

        dest.add_children(
            doc.create_element( "upnp:artist", doc.create_text_node( album.psz_artist ) ),
            doc.create_element( "upnp:class",
                                doc.create_text_node( "object.container.album.musicAlbum" ) ),
            doc.create_element( "dc:title", doc.create_text_node( album.psz_title ) ),
            doc.create_element( "dc:description", doc.create_text_node( album.psz_summary ) ) );
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

        assert( !folder.b_banned );

        const auto path = vlc::wrap_cptr( vlc_uri2path( folder.psz_mrl ), &free );
        dest.add_children(
            doc.create_element( "upnp:class", doc.create_text_node( "object.container" ) ),
            doc.create_element( "dc:title", doc.create_text_node( path.get() ) ) );
    }
};
} // namespace cds

#endif /* MLCONTAINER_HPP */
