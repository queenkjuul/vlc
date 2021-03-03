#pragma once

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_media_library.h>

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
