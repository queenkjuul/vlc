/*****************************************************************************
 * cds.cpp
 *****************************************************************************
 * Copyright © 2019 VLC authors and VideoLAN
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cds.hpp"

#include "FixedContainer.hpp"
#include "Item.hpp"
#include "MLContainer.hpp"

#include "MLFolderContainer.hpp"

namespace cds
{

template <typename T>
opt::Option<T> next_value( std::stringstream& ss, const char delim )
{
    std::string token;
    if ( !std::getline( ss, token, delim ) )
        return opt::nullopt;
    try
    {
        return static_cast<T>( std::stoull( token ) );
    }
    catch ( const std::invalid_argument& )
    {
        return opt::nullopt;
    }
};

std::tuple<unsigned, Object::ExtraId> parse_id( const std::string& id )
{
    std::stringstream ss( id );
    const auto parsed_id = next_value<unsigned>( ss, ':' );
    if ( !parsed_id.has_value() )
        throw std::invalid_argument( "Invalid id" );
    const auto parsed_ml_id = next_value<int64_t>( ss, '(' );

    opt::Option<std::string> parent = opt::nullopt;
    {
        std::string token;
        if ( std::getline( ss, token ) && !token.empty() && token.back() == ')' )
            parent = token.substr( 0, token.size() - 1 );
    }

    if ( parsed_ml_id.has_value() )
        return { parsed_id.value(), { { parsed_ml_id.value(), parent.value_or( "" ) } } };
    return { parsed_id.value(), opt::nullopt };
}

std::vector<std::unique_ptr<Object>>
init_hierarchy( vlc_medialibrary_t& ml )
{
    std::vector<std::unique_ptr<Object>> hierarchy;

    const auto add_fixed_container =
        [&]( const char* name, std::initializer_list<FixedContainer::ObjRef> children ) -> Object& {
        const int64_t id = hierarchy.size();
        for ( Object& child : children )
            child.parent_id = id;
        auto up = std::make_unique<FixedContainer>( id, -1, name, children );
        hierarchy.emplace_back( std::move( up ) );
        return *hierarchy.back();
    };

    const auto add_ml_container = [&]( auto MLHelper, const char* name, Object& child ) -> Object& {
        const int64_t id = hierarchy.size();
        child.parent_id = id;
        hierarchy.push_back( std::make_unique<MLContainer<decltype( MLHelper )>>(
            id, -1, name, ml, child ) );
        return static_cast<Object&>( *hierarchy.back() );
    };

    hierarchy.push_back( std::make_unique<FixedContainer>( 0, -1, "Home" ) );
    hierarchy.push_back( std::make_unique<Item>( 1, ml ) );

    const auto& item = static_cast<const Item&>( *hierarchy[1] );


    const auto add_ml_folder_container = [&]( ) -> Object& {
        const int64_t id = hierarchy.size();
        hierarchy.push_back( std::make_unique<MLFolderContainer>(
            id, -1, nullptr, ml, item ) );
        return static_cast<Object&>( *hierarchy.back() );
    };

    const auto add_ml_media_container = [&]( auto MLHelper, const char* name ) -> Object& {
        const int64_t id = hierarchy.size();
        hierarchy.push_back( std::make_unique<MLContainer<decltype( MLHelper )>>(
            id, -1, name, ml, item ) );
        return static_cast<Object&>( *hierarchy.back() );
    };

    static_cast<FixedContainer&>( *hierarchy[0] )
        .add_children( {
            add_fixed_container(
                "Video",
                { add_ml_media_container( ml::AllVideos{}, "All Video" ),
                  add_ml_container( ml::ShowsList{}, "Shows",
                                    add_ml_media_container( ml::ShowsMediaList{}, nullptr ) ) } ),
            add_fixed_container(
                "Music",
                {
                    add_ml_media_container( ml::AllAudio{}, "All Audio" ),
                    add_ml_container( ml::AllAlbums{}, "Albums",
                                      add_ml_media_container( ml::AlbumTracksList{}, nullptr ) ),
                    add_fixed_container(
                        "Artists",
                        { add_ml_container(
                              ml::AllArtistsList{}, "By Tracks",
                              add_ml_media_container( ml::ArtistTracksList{}, nullptr ) ),
                          add_ml_container(
                              ml::AllArtistsList{}, "By Albums",
                              add_ml_container(
                                  ml::ArtistAlbumList{}, nullptr,
                                  add_ml_media_container( ml::AlbumTracksList{}, nullptr ) ) ) } ),
                    add_fixed_container(
                        "Genres",
                        { add_ml_container(
                              ml::AllGenresList{}, "By Tracks",
                              add_ml_media_container( ml::GenreTracksList{}, nullptr ) ),
                          add_ml_container(
                              ml::AllGenresList{}, "By Albums",
                              add_ml_container(
                                  ml::GenreAlbumList{}, nullptr,
                                  add_ml_media_container( ml::AlbumTracksList{}, nullptr ) ) ) } ),
                } ),
            add_ml_container( ml::PlaylistsList{}, "Playlists",
                              add_ml_media_container( ml::PlaylistMediaList{}, nullptr ) ),
            add_ml_container( ml::AllEntryPoints{}, "Entry points", add_ml_folder_container() ),
        } );

    return hierarchy;
}
} // namespace cds
