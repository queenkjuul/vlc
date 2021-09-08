/*****************************************************************************
 * FixedContainer.hpp : Simple Container implementation
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
#ifndef FIXEDCONTAINER_HPP
#define FIXEDCONTAINER_HPP

#include "Container.hpp"

#include <vector>

namespace cds {
/// Simplest Container implementation, it is fixed in the object hierarchy and simply list other
/// Objects
struct FixedContainer : public Container
{
    using ObjRef = std::reference_wrapper<Object>;
    FixedContainer( const int64_t id,
                    const int64_t parent_id,
                    const char* name,
                    std::initializer_list<ObjRef> ) noexcept;
    FixedContainer( const int64_t id,
                    const int64_t parent_id,
                    const char* name) noexcept;

    BrowseStats browse_direct_children( xml::Element&,
                                        BrowseParams,
                                        const Object::ExtraId&,
                                        const ClientProfile& ) const final;

    void add_children(std::initializer_list<ObjRef> l);
    private:
    std::vector<ObjRef> children;
  };
}

#endif /* FIXEDCONTAINER_HPP */
