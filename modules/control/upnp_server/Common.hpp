#pragma once

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_media_library.h>


struct TranscodeProfile
{
    const char* psz_name;
    vlc_ml_media_type_t i_type;

    // We only use transcode for resolution for the PoC, so this is quite empty for the moment
    struct {
      unsigned width;
      unsigned height;
    } resolution;
    const char *mux;
};

struct TranscodeProfileList {
  unsigned u_size;
  const TranscodeProfile *p_profiles;
};
