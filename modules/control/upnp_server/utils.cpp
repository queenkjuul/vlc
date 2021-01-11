#include "utils.hpp"

#include <upnp.h>

#include <sstream>
#include <functional>

namespace utils
{
std::string file_extension( const std::string& file ) noexcept
{
    auto pos = file.find_last_of( '.' );
    if ( pos == std::string::npos )
        return {};
    return file.substr( pos + 1 );
}

std::string make_transcode_line( const TranscodeProfile& profile,
                                 const vlc_ml_media_t& media ) noexcept
{
    const vlc_ml_media_track_t* vtrack = nullptr;
    const vlc_ml_media_track_t* atrack = nullptr;
    for ( unsigned i = 0; i < media.p_tracks->i_nb_items; ++i )
    {
        const auto& track = media.p_tracks->p_items[i];
        switch ( track.i_type )
        {
        case VLC_ML_TRACK_TYPE_AUDIO:
            atrack = &track;
            break;
        case VLC_ML_TRACK_TYPE_VIDEO:
            vtrack = &track;
            break;
        default:
            break;
        }
    }
    std::stringstream out;
    out << "sout=#transcode{"
        << "vcodec=" << ( vtrack ? vtrack->psz_codec : "none" ) << ','
        << "acodec=" << ( atrack ? atrack->psz_codec : "none" ) << ",scodec=none";

    if ( vtrack )
    {
        out << ",scale=Auto,vb=800,width=" << profile.resolution.width
            << ",height=" << profile.resolution.height;
    }
    out << '}';
    return out.str();
}

std::vector<MediaTrackRef> get_media_tracks(const vlc_ml_media_t& media, vlc_ml_track_type_t type) noexcept
{
  std::vector<MediaTrackRef> ret;

  if (media.p_tracks == nullptr)
    return ret;
  for (unsigned i = 0; i < media.p_tracks->i_nb_items; ++i)
  {
    const auto& track = media.p_tracks->p_items[i];
    if (track.i_type == type)
      ret.emplace_back(track);
  }
  return ret;
}

std::vector<MediaFileRef> get_media_files(const vlc_ml_media_t& media, vlc_ml_file_type_t type) noexcept
{
  std::vector<MediaFileRef> ret;

  if (media.p_files == nullptr)
    return ret;
  for (unsigned i = 0; i < media.p_files->i_nb_items; ++i)
  {
    const auto& file = media.p_files->p_items[i];
    if (file.i_type == type)
      ret.emplace_back(file);
  }
  return ret;
}

std::string get_server_url() noexcept 
{
    // TODO support ipv6
    const std::string addr = UpnpGetServerIpAddress();
    const std::string port = std::to_string( UpnpGetServerPort() );
    return "http://" + addr + ':' + port + '/';
}
} // namespace utils
