#pragma once

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_media_library.h>

#include <vector>

struct TranscodeProfile
{
    const char* psz_name;
    int i_conversion_quality;
    const std::vector<vlc_fourcc_t> codecs_fallbacks;
};

struct TranscodeProfileList {
  unsigned u_size;
  const TranscodeProfile *p_profiles;
};
