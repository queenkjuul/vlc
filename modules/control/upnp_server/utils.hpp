/*****************************************************************************
 * utils.hpp : UPnP server utils
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
#ifndef UTILS_HPP
#define UTILS_HPP

#include "Profiles.hpp"
#include <vlc_media_library.h>

#include <upnp.h>
#include <UpnpExtraHeaders.h>

#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace utils
{
std::string file_extension( const std::string& file );

struct MimeType
{
    std::string media_type;
    std::string file_type;

    std::string combine() const noexcept { return media_type + '/' + file_type; }
};

MimeType get_mimetype( vlc_ml_media_type_t type, const std::string& file_extension ) noexcept;

std::string get_server_url();
std::string get_root_dir() noexcept;

std::string addr_to_string(const sockaddr_storage *addr);

template <typename T>
using ConstRef = std::reference_wrapper<const T>;

using MediaTrackRef = ConstRef<vlc_ml_media_track_t>;
std::vector<MediaTrackRef> get_media_tracks(const vlc_ml_media_t& media, vlc_ml_track_type_t type) noexcept;

using MediaFileRef = ConstRef<vlc_ml_file_t>;
std::vector<MediaFileRef> get_media_files(const vlc_ml_media_t& media, vlc_ml_file_type_t) noexcept;

std::string get_dlna_extra_protocol_info( const std::string& dlna_profile,
                                          const TranscodeProfile* );
std::string infer_dlna_profile_name( const std::string& extension );

time_t parse_dlna_npt_range( const char* value );

namespace http
{
UpnpExtraHeaders* get_hdr( UpnpListHead* list, const std::string name );
void add_response_hdr( UpnpListHead* list, const std::pair<std::string, std::string> resp );
const char* get_extra_header( UpnpListHead* list, const char* name );
} // namespace http

} // namespace utils

#endif /* UTILS_HPP */
