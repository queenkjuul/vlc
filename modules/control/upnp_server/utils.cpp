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

std::string album_thumbnail_url( const vlc_ml_album_t& album ) noexcept
{

    const auto thumbnail_extension =
        file_extension( std::string( album.thumbnails[VLC_ML_THUMBNAIL_SMALL].psz_mrl ) );
    return get_server_url() + "thumbnail/small/album/" + std::to_string( album.i_id ) + "." +
           thumbnail_extension;
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

std::string get_mimetype( vlc_ml_media_type_t type, const std::string& file_extension ) noexcept
{
    const char* mime_end = file_extension.c_str();
    if ( file_extension == "mkv" )
        mime_end = "x-matroska";
    switch ( type )
    {
    case VLC_ML_MEDIA_TYPE_AUDIO:
      return std::string("audio/") + mime_end;
    case VLC_ML_MEDIA_TYPE_UNKNOWN: // Intended pass through
      assert(!"Unknown media type");
    case VLC_ML_MEDIA_TYPE_VIDEO:
      return std::string("video/") + mime_end;
    default:
      assert(!"Invalid media type");
  }
}

std::string get_root_dir() noexcept
{
    std::stringstream ret;

    char* path = config_GetSysPath( VLC_PKG_DATA_DIR, NULL );

    ret << path << "/upnp_server/";

    free( path );
    return ret.str();
}
} // namespace utils
