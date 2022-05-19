/*****************************************************************************
 * MLFolderContainer.hpp : MediaLibrary IFolder container
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
#ifndef MLFOLDERCONTAINER_HPP
#define MLFOLDERCONTAINER_HPP

#include "Container.hpp"
#include "Item.hpp"

#include <vlc_media_library.h>
#include <vlc_url.h>
#include <vlc_cxx_helpers.hpp>

#include "../ml.hpp"
#include "../utils.hpp"

namespace cds
{
class MLFolderContainer : public Container
{
    vlc_medialibrary_t& ml;
    const Item& child;

    static void dump_folder_metadata(xml::Element& dest, const vlc_ml_folder_t& folder) {
        xml::Document& doc = dest.owner;
        auto path = vlc::wrap_cptr(vlc_uri2path(folder.psz_mrl), &free);

        // Only keep the last folder from the path
        const char *folder_name = nullptr;
        if ( path != nullptr && strlen( path.get() ) > 0 )
        {
#ifdef _WIN32
            const char sep = '\\';
#else
            const char sep = '/';
#endif

            for ( auto i = strlen( path.get() ) - 1; i > 0; --i )
            {
                if ( path.get()[i] == sep )
                    path.get()[i] = '\0';
                else
                    break;
            }
            folder_name = strrchr( path.get(), '/' ) + 1;
        }

        dest.add_children(
            doc.create_element( "dc:title", doc.create_text_node( folder_name ? folder_name : path.get() ) ),
            doc.create_element( "upnp:class", doc.create_text_node( "object.container" ) ) );
    }

    public:
    MLFolderContainer( int64_t id,
                       int64_t parent_id,
                       const char* name,
                       vlc_medialibrary_t& ml,
                       const Item& child )
        : Container( id, parent_id, name, 0 )
        , ml( ml )
        , child( child )
    {
    }

    void dump_metadata( xml::Element& dest, const Object::ExtraId& extra, const ClientProfile& profile ) const final
    {
        if ( !extra.has_value() )
        {
            Container::dump_metadata( dest, opt::nullopt, profile);
            return;
        }
        const auto folder = ml::Folder::get( ml, extra->ml_id );

        if ( folder != nullptr )
        {
          dump_folder_metadata(dest, *folder);
        }
    }

    BrowseStats browse_direct_children( xml::Element& dest,
                                        const BrowseParams params,
                                        const Object::ExtraId& extra,
                                        const ClientProfile& profile ) const final
    {
        const vlc_ml_query_params_t query_params = { nullptr, params.requested, params.offset,
                                                     VLC_ML_SORTING_DEFAULT, true, true };
        assert( extra.has_value() );

        const auto folder = ml::Folder::get( ml, extra->ml_id );
        assert( folder != nullptr );

        xml::Document& doc = dest.owner;

        vlc_ml_folder_list_t* list = nullptr;
        auto status =
            vlc_ml_list( &ml, VLC_ML_LIST_SUBFOLDERS, &query_params, extra->ml_id, &list );
        assert( status == VLC_SUCCESS );
        for ( auto i = 0u; i < list->i_nb_items; ++i )
        {
            const auto& folder = list->p_items[i];
            auto elem = create_object_element(
                doc, ExtraIdData{ folder.i_id, get_dynamic_id( extra ) } );
            dump_folder_metadata(elem, folder);
            dest.add_child( std::move( elem ) );
        }

        const size_t listed_subfolders = list->i_nb_items;
        vlc_ml_release( list );

        size_t total_subfolders;
        status = vlc_ml_list( &ml, VLC_ML_COUNT_SUBFOLDERS, &query_params, extra->ml_id, &total_subfolders );


        const auto media_list = ml::MediaFolderList::list(ml, &query_params, int64_t(extra->ml_id));

        if ( media_list )
        {
            for ( auto i = 0u; i < media_list->i_nb_items; ++i )
            {
                const auto& media = media_list->p_items[i];
                auto elem = child.create_object_element(
                    doc, ExtraIdData{ child.id, get_dynamic_id( extra ) } );
                Item::dump_mlobject_metadata( elem, media, ml, profile );
                dest.add_child( std::move( elem ) );
            }
        }

        return { listed_subfolders, total_subfolders };
    }
};
} // namespace cds

#endif /* MLFOLDERCONTAINER_HPP */
