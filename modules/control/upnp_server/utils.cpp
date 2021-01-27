#include "utils.hpp"

#include <upnp.h>
#include "../stream_out/dlna/dlna.hpp"

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

std::string thumbnail_url( const vlc_ml_media_t& media,
                           vlc_ml_thumbnail_size_t size ) noexcept
{
  std::stringstream ss;
  const std::string s_size = size == VLC_ML_THUMBNAIL_SMALL ? "small" : "banner";
  const auto & thumbnail = media.thumbnails[size];

    const auto thumbnail_extension = file_extension( std::string( thumbnail.psz_mrl ) );
    ss << get_server_url() << "thumbnail/"
       << ( size == VLC_ML_THUMBNAIL_SMALL ? "small" : "banner" ) << "/media/" << media.i_id << '.'
       << thumbnail_extension;

    return ss.str();
}

std::string album_thumbnail_url( const vlc_ml_album_t& album ) noexcept
{
    const auto& thumbnail = album.thumbnails[VLC_ML_THUMBNAIL_SMALL];
    if ( thumbnail.i_status != VLC_ML_THUMBNAIL_STATUS_AVAILABLE )
        return "";
    const auto thumbnail_extension = file_extension( std::string( thumbnail.psz_mrl ) );
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

MimeType get_mimetype( vlc_ml_media_type_t type, const std::string& file_extension ) noexcept
{
    const char* mime_end = file_extension.c_str();
    if ( file_extension == "mkv" )
        mime_end = "x-matroska";
    switch ( type )
    {
    case VLC_ML_MEDIA_TYPE_AUDIO:
      return {"audio", mime_end};
    case VLC_ML_MEDIA_TYPE_UNKNOWN: // Intended pass through
      assert(!"Unknown media type");
    case VLC_ML_MEDIA_TYPE_VIDEO:
      return {"video", mime_end};
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

//TODO We should change that to a better profile selection using profiles in dlna.hpp
// as soon as more info on media tracks are available in the medialibrary
std::string infer_dlna_profile_name(const std::string& extension) noexcept
{
    std::string profile;
    if (extension == "mp4")
        profile = "AVC_MP4_EU";
    else if (extension == "x-matroska")
        profile = "MKV";
    else if (extension == "avi")
        profile = "AVI";
    else if (extension == "mp3")
        profile = "MP3";
    else if (extension == "jpg")
        profile = "JPG";
    else if (extension == "png")
        profile = "PNG";
    return profile;
}

std::string get_dlna_extra_protocol_info( const std::string& file_type, bool transcode ) noexcept
{
    std::ostringstream ret;

    const auto dlna_profile = transcode ? "MKV" : infer_dlna_profile_name(file_type);

    if ( !dlna_profile.empty() )
        ret << "DLNA.ORG_PN=" << dlna_profile << ";";

    dlna_org_flags_t flags = DLNA_ORG_FLAG_STREAMING_TRANSFER_MODE |
                             DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE |
                             DLNA_ORG_FLAG_CONNECTION_STALL | DLNA_ORG_FLAG_DLNA_V15;
    dlna_org_operation_t op;
    dlna_org_conversion_t conv;
    if ( transcode )
    {
        flags = flags | DLNA_ORG_FLAG_TIME_BASED_SEEK;
        op = DLNA_ORG_OPERATION_TIMESEEK;
        conv = DLNA_ORG_CONVERSION_TRANSCODED;
    }
    else
    {
        op = DLNA_ORG_OPERATION_RANGE;
        conv = DLNA_ORG_CONVERSION_NONE;
    }

    char dlna_info[448];
    sprintf( dlna_info, "%s=%.2x;%s=%d;%s=%.8x%.24x", "DLNA.ORG_OP", op, "DLNA.ORG_CI", conv,
             "DLNA.ORG_FLAGS", flags, 0 );
    ret << dlna_info;
    return ret.str();
}
} // namespace utils
