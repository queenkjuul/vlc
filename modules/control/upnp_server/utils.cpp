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

#include "utils.hpp"

#include "../stream_out/dlna/dlna.hpp"

#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#include <sstream>
#include <functional>
#include <algorithm>
#include <iostream>
#include <vlc_dialog.h>

namespace utils
{
std::string file_extension( const std::string& file )
{
    auto pos = file.find_last_of( '.' );
    if ( pos == std::string::npos )
        return {};
    return file.substr( pos + 1 );
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

std::string get_server_url()
{
    // TODO support ipv6
    const std::string addr = UpnpGetServerIpAddress();
    const std::string port = std::to_string( UpnpGetServerPort() );
    return "http://" + addr + ':' + port + '/';
}

std::string addr_to_string(const sockaddr_storage *addr) {
  const void *ip;
  if (addr->ss_family == AF_INET6) {
      ip = &reinterpret_cast<const struct sockaddr_in6*>( addr )->sin6_addr;
  }
  else
  {
      ip = &reinterpret_cast<const struct sockaddr_in*>( addr )->sin_addr;
  }

  char buff[INET6_ADDRSTRLEN];
  inet_ntop(addr->ss_family, ip, buff, sizeof(buff));
  return buff;
}

MimeType get_mimetype( vlc_ml_media_type_t type, const std::string& file_extension ) noexcept
{
    const char* mime_end = file_extension.c_str();
    // special case for the transcode muxer to be widely accepted by a majority of players.
    if ( file_extension == "ts" )
        mime_end = "mpeg";
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
    assert( path );

    ret << path << "/upnp_server/";

    free( path );
    return ret.str();
}

// TODO We should change that to a better profile selection using profiles in dlna.hpp
// as soon as more info on media tracks are available in the medialibrary
std::string infer_dlna_profile_name( const std::string& extension )
{
    if ( extension == "mp4" )
        return "AVC_MP4_EU";

    std::string profile;
    std::transform( std::begin( extension ), std::end( extension ), std::begin( profile ),
                    ::toupper );
    return profile;
}

std::string get_dlna_extra_protocol_info( const std::string& file_type )
{
    std::ostringstream ret;

    const auto dlna_profile = infer_dlna_profile_name(file_type);

    if ( !dlna_profile.empty() )
        ret << "DLNA.ORG_PN=" << dlna_profile << ";";

    dlna_org_flags_t flags = DLNA_ORG_FLAG_STREAMING_TRANSFER_MODE |
                             DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE |
                             DLNA_ORG_FLAG_CONNECTION_STALL | DLNA_ORG_FLAG_DLNA_V15;
    dlna_org_operation_t op;
    dlna_org_conversion_t conv;

    op = DLNA_ORG_OPERATION_RANGE;
    conv = DLNA_ORG_CONVERSION_NONE;

    char dlna_info[448];
    sprintf( dlna_info, "%s=%.2x;%s=%d;%s=%.8x%.24x", "DLNA.ORG_OP", op, "DLNA.ORG_CI", conv,
             "DLNA.ORG_FLAGS", flags, 0 );
    ret << dlna_info;
    return ret.str();
}

namespace http {
UpnpExtraHeaders* get_hdr( UpnpListHead* list, const std::string name )
{
    for ( auto* it = UpnpListBegin( list ); it != UpnpListEnd( list );
          it = UpnpListNext( list, it ) )
    {
        UpnpExtraHeaders* hd = reinterpret_cast<UpnpExtraHeaders*>( it );
        std::string hdr_name = UpnpExtraHeaders_get_name_cstr( hd );
//        std::cerr << hdr_name << ": " << UpnpExtraHeaders_get_value_cstr( hd ) << "\n";
        std::transform( std::begin( hdr_name ), std::end( hdr_name ), std::begin( hdr_name ),
                        ::tolower );
        if ( hdr_name == name )
        {
            return hd;
        }
    }
    return nullptr;
}

void add_response_hdr( UpnpListHead* list, const std::pair<std::string, std::string> resp )
{

    auto* hdr = get_hdr( list, resp.first );
    const auto resp_str = resp.first + ": " + resp.second;
 //   std::cerr << "RESP: " << resp_str << '\n';
    if ( !hdr )
    {
        hdr = UpnpExtraHeaders_new();
    }
    UpnpExtraHeaders_set_resp( hdr, resp_str.c_str() );
    UpnpListInsert( list, UpnpListEnd( list ),
                    const_cast<UpnpListHead*>( UpnpExtraHeaders_get_node( hdr ) ) );
}

const char* get_extra_header( UpnpListHead* list, const char* name )
{
    auto hdr = get_hdr( list, name );
    return hdr != nullptr ? UpnpExtraHeaders_get_value_cstr( hdr ) : nullptr;
}
}
} // namespace utils
