/*****************************************************************************
 * Clients.cpp
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
#include "upnp_server.hpp"

#include <vlc_common.h>
#include <vlc_codec.h>

#include "utils.hpp"

#include "../stream_out/renderer_common.hpp"

#include <sstream>
#include <unordered_map>

const std::unordered_map<std::string, ClientProfile> CLIENT_PROFILES = {

  { "PLAYSTATION 3",
    {
      {
        {
          "video_ps3",
          CONVERSION_QUALITY_LOW,
          VLC_ML_MEDIA_TYPE_VIDEO,
          {VLC_CODEC_H264},
          VLC_CODEC_A52,
          {"ts", "ts", "AVC_TS_EU_ISO"}
        },
        {
          "audio_ps3",
          CONVERSION_QUALITY_HIGH,
          VLC_ML_MEDIA_TYPE_AUDIO,
          {},
          VLC_CODEC_S16B,
          {"raw", "L16;rate=48000;channels=2", "L16"}
        }
      },
      false // native_resource_first
    }
  }
};

const ClientProfile DEFAULT_PROFILE = {
  {
    {
      "av_high",
      CONVERSION_QUALITY_HIGH,
      VLC_ML_MEDIA_TYPE_VIDEO,
      {VLC_CODEC_H264, VLC_CODEC_VP8},
      VLC_CODEC_MP4A,
      {"ts", "ts", "AVC_TS_EU_ISO"}
    },
    {
      "a_high",
      CONVERSION_QUALITY_HIGH,
      VLC_ML_MEDIA_TYPE_AUDIO,
      {},
      VLC_CODEC_VORBIS,
      {"ogg", "ogg", "x-vorbis"}
    },
    {
      "av_low",
      CONVERSION_QUALITY_LOW,
      VLC_ML_MEDIA_TYPE_VIDEO,
      {VLC_CODEC_H264, VLC_CODEC_VP8},
      VLC_CODEC_MP4A,
      {"ts", "ts", "AVC_TS_EU_ISO"}
    },
    {
      "a_low",
      CONVERSION_QUALITY_LOW,
      VLC_ML_MEDIA_TYPE_AUDIO,
      {},
      VLC_CODEC_MP3,
      {"ts", "ts", "MP3X"}
    }
  }
};

const ClientProfile& intf_sys_t::default_profile() noexcept { return DEFAULT_PROFILE; }

const ClientProfile& intf_sys_t::profile_from_headers( UpnpListHead* headers,
                                                       const char* user_agent )
{
    const char* av_client_info = utils::http::get_extra_header( headers, "x-av-client-info" );

    std::string key;

    // Extract the "mn" field from the header value
    if ( av_client_info )
    {
        key = av_client_info;
        const auto name_idx = key.find("mn=\"");
        if (name_idx != std::string::npos) {
          const char * begin = key.c_str() + name_idx + 4;
          const char * end= strchr(begin, '\"');
          if (end != nullptr) {
            key = key.substr(name_idx + 4, end - begin);
          }
        }
    }
    else if ( user_agent )
        key = user_agent;

    if ( CLIENT_PROFILES.find( key ) != std::end( CLIENT_PROFILES ) )
        return CLIENT_PROFILES.at( key );
    return DEFAULT_PROFILE;
}
