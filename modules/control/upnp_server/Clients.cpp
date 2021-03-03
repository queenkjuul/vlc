#include "upnp_server.h"

#include <vlc_common.h>
#include <vlc_codec.h>

#include "utils.hpp"

#include "../stream_out/renderer_common.hpp"

#include <sstream>

const std::unordered_map<std::string, ClientProfile> intf_sys_t::CLIENT_PROFILES = {

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
          // TODO find a goddamn working codec + muxer combo for the ps3
          "audio_ps3",
          CONVERSION_QUALITY_LOW,
          VLC_ML_MEDIA_TYPE_AUDIO,
          {},
          VLC_CODEC_MP3,
          {"ts", "ts", "MP3X"}
        }
      },
      false // native_resource_first
    }
  }
};

const ClientProfile intf_sys_t::DEFAULT_PROFILES = {
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


const ClientProfile& intf_sys_t::profile_from_headers( UpnpListHead* headers,
                                                       const char* user_agent ) noexcept
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
    return DEFAULT_PROFILES;
}
