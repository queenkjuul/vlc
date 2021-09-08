/*****************************************************************************
 * Container.hpp : CDS Container interface
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
#ifndef CONTAINER_HPP
#define CONTAINER_HPP

#include "Object.hpp"

namespace cds
{
/// Opaque CDS container type
/// Specs: http://www.upnp.org/specs/av/UPnP-av-ContentDirectory-v3-Service-20080930.pdf
/// "2.2.8 - Container"
class Container : public Object
{
    protected:
    size_t child_count;

    public:
    struct BrowseParams
    {
        uint32_t offset;
        uint32_t requested;
    };

    struct BrowseStats
    {
        size_t result_count;
        size_t total_matches;
    };

    Container( const int64_t id,
               const int64_t parent_id,
               const char* name,
               size_t child_count ) noexcept
        : Object( id, parent_id, name, Object::Type::Container )
        , child_count( child_count )
    {
    }

    virtual void dump_metadata( xml::Element& dest, const Object::ExtraId&, const ClientProfile&) const override
    {
        dest.set_attribute( "childCount", std::to_string( child_count ).c_str() );

        xml::Document& doc = dest.owner;
        dest.add_child(
            doc.create_element( "upnp:class", doc.create_text_node( "object.container" ) ) );
    }

    /// Go through all the container children and dump them to the given xml element
    virtual BrowseStats browse_direct_children( xml::Element&,
                                                BrowseParams,
                                                const Object::ExtraId&,
                                                const ClientProfile& ) const = 0;
};
} // namespace cds

#endif /* CONTAINER_HPP */
