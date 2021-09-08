/*****************************************************************************
 * Profiles.hpp : Upnp clients profiles
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
#ifndef PROFILES_HPP
#define PROFILES_HPP

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>

#include <vector>
#include <string>

struct TranscodeProfile
{
    std::string name;
    int conversion_quality; // see stream_out/renderer_common.hpp
    int media_type;
    const std::vector<vlc_fourcc_t> vcodecs_fallbacks;
    vlc_fourcc_t acodec;
    struct
    {
        const char* name;
        const char* file_extension;
        const char* dlna_profile_name;
    } mux;
};

struct ClientProfile {
  std::vector<TranscodeProfile> transcode_profiles;

  // Set to false when the client only play the first media resource and ignore the others.
  bool native_resource_first = true;
};

#endif /* PROFILES_HPP */
