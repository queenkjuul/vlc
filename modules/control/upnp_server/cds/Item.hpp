/*****************************************************************************
 * Item.hpp : CDS Item interface
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
#ifndef ITEM_HPP
#define ITEM_HPP

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>

#include <vlc_media_library.h>

#include "Object.hpp"

namespace cds
{
/// This is a dynamic object representing a medialibrary media
/// It expect to receive the medialibrary id of the media it should represent in its Extra ID, with
/// that, a single instance of Item can effectively represent all medialibrary medias
class Item : public Object
{
    vlc_medialibrary_t &m_medialib;

  public:
    Item(const int64_t id, vlc_medialibrary_t &) noexcept;
    void dump_metadata(xml::Element &, const Object::ExtraId &) const final;

    static void
    dump_mlobject_metadata(xml::Element &dest, const vlc_ml_media_t &media, vlc_medialibrary_t &ml);
};
} // namespace cds

#endif /* ITEM_HPP */
