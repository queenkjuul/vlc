#pragma once

#include "Common.hpp"

#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace utils
{
std::string file_extension( const std::string& file ) noexcept;

struct MimeType
{
    std::string media_type;
    std::string file_type;

    std::string combine() const noexcept { return media_type + '/' + file_type; }
};

MimeType get_mimetype( vlc_ml_media_type_t type, const std::string& file_extension ) noexcept;

std::string thumbnail_url( const vlc_ml_media_t& media,
                           vlc_ml_thumbnail_size_t size ) noexcept;
std::string album_thumbnail_url(const vlc_ml_album_t &) noexcept;

std::string get_server_url() noexcept;
std::string get_root_dir() noexcept;

template <typename T>
using ConstRef = std::reference_wrapper<const T>;

using MediaTrackRef = ConstRef<vlc_ml_media_track_t>;
std::vector<MediaTrackRef> get_media_tracks(const vlc_ml_media_t& media, vlc_ml_track_type_t type) noexcept;

using MediaFileRef = ConstRef<vlc_ml_file_t>;
std::vector<MediaFileRef> get_media_files(const vlc_ml_media_t& media, vlc_ml_file_type_t) noexcept;

std::string get_dlna_extra_protocol_info( const std::string& dlna_profile,
                                          bool transcode ) noexcept;
std::string infer_dlna_profile_name( const std::string& extension ) noexcept;

template <typename Rep, typename Pediod = std::ratio<1>>
std::string duration_to_string( const char* fmt, std::chrono::duration<Rep, Pediod> duration )
{
    char ret[32] = { 0 };
    using namespace std::chrono;
    // Substract 1 hour because std::localtime starts at 1 AM
    const auto sec = duration_cast<seconds>( duration - 1h ).count();
    const auto size = std::strftime( ret, sizeof( ret ), fmt, std::localtime( &sec ) );
    return { ret, size };
}
} // namespace utils
