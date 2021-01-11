#pragma once

#include "Common.hpp"

#include <string>
#include <vector>

namespace utils
{
std::string file_extension( const std::string& file ) noexcept;
std::string make_transcode_line( const TranscodeProfile&, const vlc_ml_media_t& ) noexcept;

std::string get_server_url() noexcept;

template <typename T>
using ConstRef = std::reference_wrapper<const T>;

using MediaTrackRef = ConstRef<vlc_ml_media_track_t>;
std::vector<MediaTrackRef> get_media_tracks(const vlc_ml_media_t& media, vlc_ml_track_type_t type) noexcept;

using MediaFileRef = ConstRef<vlc_ml_file_t>;
std::vector<MediaFileRef> get_media_files(const vlc_ml_media_t& media, vlc_ml_file_type_t) noexcept;

}
