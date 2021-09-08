/*****************************************************************************
 * FileHandler.cpp
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "FileHandler.hpp"

#include "utils.hpp"

#include <vlc_common.h>

#include <vlc_addons.h>
#include <vlc_cxx_helpers.hpp>
#include <vlc_interface.h>
#include <vlc_player.h>
#include <vlc_rand.h>
#include <vlc_stream.h>
#include <vlc_stream_extractor.h>
#include <vlc_url.h>
#include <vlc_fourcc.h>

#include <ctime>
#include <sstream>
#include <algorithm>

#include "stream_out/renderer_common.hpp"

//static constexpr char DLNA_TRANSFER_MODE[] = "transfermode.dlna.org";
static constexpr char DLNA_CONTENT_FEATURE[] = "contentfeatures.dlna.org";
static constexpr char DLNA_TIME_SEEK_RANGE[] = "timeseekrange.dlna.org";

/// Convenient C++ replacement of vlc_ml_file_t to not have to deal with allocations
struct MLFile
{
    std::string mrl;
    int64_t size;
    time_t last_modification;
};

/// Usual filesystem FileHandler implementation, this is the most commonly used FileHandler, for
/// non-transcoded local medias, thumbnails and subs.
struct MLFileHandler : FileHandler
{
    MLFile file;
    utils::MimeType mime_type;

    std::unique_ptr<stream_t, decltype( &vlc_stream_Delete )> stream = { nullptr,
                                                                         &vlc_stream_Delete };

    MLFileHandler( MLFile&& file, utils::MimeType&& mime_type )
        : file( std::move( file ) )
        , mime_type( std::move( mime_type ) )
    {
    }

    bool get_info( UpnpFileInfo& info ) final
    {
        UpnpFileInfo_set_ContentType( &info, mime_type.combine().c_str() );
        UpnpFileInfo_set_FileLength( &info, file.size );
        UpnpFileInfo_set_LastModified( &info, file.last_modification );

        // const_cast is expected by the design of the upnp api as it only serves const list heads
        // FIXME: see if there's no way to patch that in libupnp, we shouldn't have to break const
        // to do something so usual.
        auto* head = const_cast<UpnpListHead*>( UpnpFileInfo_get_ExtraHeadersList( &info ) );
        utils::http::add_response_hdr( head, { DLNA_CONTENT_FEATURE, "DLNA.ORG_OP=01" } );
        return true;
    }

    bool open( intf_thread_t* parent ) final
    {
        stream = vlc::wrap_cptr( vlc_stream_NewMRL( VLC_OBJECT( parent ), file.mrl.c_str() ),
                                 &vlc_stream_Delete );
        return stream != nullptr;
    }

    size_t read( uint8_t buffer[], size_t buffer_len ) noexcept final
    {
        return vlc_stream_Read( stream.get(), buffer, buffer_len );
    }

    bool seek( SeekType type, off_t offset ) noexcept final
    {
        uint64_t real_offset;
        switch ( type )
        {
        case SeekType::Current:
            real_offset = vlc_stream_Tell( stream.get() ) + offset;
            break;
        case SeekType::End:
            if ( vlc_stream_GetSize( stream.get(), &real_offset ) != VLC_SUCCESS )
            {
                return false;
            }
            real_offset += offset;
            break;
        case SeekType::Set:
            real_offset = offset;
            break;
        default:
            return false;
        }

        return vlc_stream_Seek( stream.get(), real_offset ) == 0;
    }
};

/// Wrap a player inside a VLC object, to be able to set a vlc var to communicate the FIFO address
struct PlayerWrapper
{
    vlc_object_t obj;
    vlc_player_t* player;
};

struct TrandscodedMediaFileHandler : FileHandler
{
    utils::MimeType mime_type;
    const TranscodeProfile& profile;

    ml::Media::Ptr media;
    time_t media_begin = 0;

    PlayerWrapper *player_wrapper;

    TranscodeFifo fifo;

    TrandscodedMediaFileHandler( utils::MimeType&& mime,
                                 const TranscodeProfile& profile,
                                 decltype( media )&& media )
        : mime_type( std::move( mime ) )
        , profile( profile )
        , media( std::move( media ) )
    {
    }

    ~TrandscodedMediaFileHandler()
    {
        if ( player_wrapper == nullptr )
            return;
        if ( player_wrapper->player != nullptr )
            vlc_player_Delete( player_wrapper->player );
        vlc_object_delete( VLC_OBJECT( player_wrapper ) );
    }

    bool get_info( UpnpFileInfo& info ) final
    {
        assert( media != nullptr );
        const auto main_files = utils::get_media_files( *media, VLC_ML_FILE_TYPE_MAIN );
        if ( main_files.empty() )
            return false;

        UpnpFileInfo_set_ContentType( &info, mime_type.combine().c_str() );
        UpnpFileInfo_set_LastModified( &info, main_files.front().get().i_last_modification_date );

        // We can't predict the media size before transcoding.
        UpnpFileInfo_set_FileLength( &info, -1 );

        // const_cast is expected by the design of the upnp api as it only serves const list heads
        // FIXME: see if there's no way to patch that in libupnp, we shouldn't have to break const
        // to do something so usual.
        auto* head = const_cast<UpnpListHead*>( UpnpFileInfo_get_ExtraHeadersList( &info ) );

        // This header is used by some DLNA clients to seek, it's the only valid way we have now to
        // seek while transcoding. We get the starting npt in a HTTP GET request, and the
        // transcoding pipeline is recreated at the requested time
        const char* time_seek_range = utils::http::get_extra_header( head, DLNA_TIME_SEEK_RANGE );
        if ( time_seek_range != nullptr )
        {
            const time_t media_begin = utils::parse_dlna_npt_range( time_seek_range );
            this->media_begin = std::min( media_begin, static_cast<time_t>( media->i_duration ) );
            utils::http::add_response_hdr(
                head, { DLNA_TIME_SEEK_RANGE, std::to_string( media_begin / 1000. ) + '-' } );
        }

        utils::http::add_response_hdr( head, { DLNA_CONTENT_FEATURE, "DLNA.ORG_OP=10" } );
        //utils::http::add_response_hdr( head, { DLNA_TRANSFER_MODE, "Streaming" } );
        //utils::http::add_response_hdr( head, { "Pragma", "no-cache" } );
        //utils::http::add_response_hdr( head, { "Cache-Control", "no-cache" } );
        return true;
    }

    bool open( intf_thread_t* parent ) final
    {
        player_wrapper =
            static_cast<PlayerWrapper*>( vlc_object_create( parent, sizeof( *player_wrapper ) ) );
        if (player_wrapper == nullptr)
          return false;

        player_wrapper->player = vlc_player_New( VLC_OBJECT( player_wrapper ),
                                                 VLC_PLAYER_LOCK_NORMAL, nullptr, nullptr );
        if ( player_wrapper->player == nullptr )
        {
            return false;
        }

        const auto main_files = utils::get_media_files( *media, VLC_ML_FILE_TYPE_MAIN );
        if ( main_files.empty() )
            return false;
        const vlc_ml_file_t& main_file = main_files.front();

        auto item =
            vlc::wrap_cptr( vlc_ml_get_input_item_by_mrl( parent->p_sys->p_ml, main_file.psz_mrl ),
                            &input_item_Release );
        if ( item == nullptr )
        {
            return false;
        }

        // VLC transcode pipeline building
        std::stringstream transcode_line;
        // If a specific starting time is set, use the proxy to avoid sending unhandled random
        // access discontinuities. See upnp-server.h
        if ( media_begin != 0 )
        {
            transcode_line << "sout=#upnp-sout-proxy:transcode{";
        }
        else
        {
            transcode_line << "sout=#transcode{";
        }

        switch(profile.media_type)
        {
        case VLC_ML_MEDIA_TYPE_VIDEO:
            transcode_line << get_video_transcoding_params( VLC_OBJECT( parent ), profile );
            // Fall-through
        case VLC_ML_MEDIA_TYPE_AUDIO:
            transcode_line << get_audio_transcoding_params( VLC_OBJECT( parent ), profile );
            break;
        }
        transcode_line << "}:std{access=upnp-out,mux=" << profile.mux.name << "}";

        const auto transcode_line_str = transcode_line.str();
        int res = input_item_AddOption( item.get(), transcode_line_str.c_str(),
                                        VLC_INPUT_OPTION_TRUSTED );

        if ( res != VLC_SUCCESS )
        {
            msg_Err( parent, "Can't append transcode chain to the player" );
            return false;
        }

        msg_Info( parent, "Transcoding '%s' with profile '%s'", media->psz_title,
                  profile.name.c_str() );

        var_Create( VLC_OBJECT( player_wrapper ), TranscodeFifo::VAR_NAME, VLC_VAR_ADDRESS );
        var_SetAddress( VLC_OBJECT( player_wrapper ), TranscodeFifo::VAR_NAME, &fifo );

        vlc_player_t *player = player_wrapper->player;
        vlc_player_Lock( player );

        const bool ret = vlc_player_SetCurrentMedia( player, item.get() ) == VLC_SUCCESS;

        if ( media_begin > 0 )
            vlc_player_SetTime( player, VLC_TICK_FROM_MS( media_begin ) );

        vlc_player_Start( player );

        vlc_player_Unlock( player );
        return ret;
    }

    size_t read( uint8_t buffer[], size_t buffer_len ) noexcept final
    {
        return fifo.read( buffer, buffer_len );
    }

    // Byte based seeking isn't and can't be supported in transcoding.
    bool seek( SeekType, off_t ) noexcept final { return false; }

  private:
  std::string get_video_transcoding_params( vlc_object_t* parent,
                                          const TranscodeProfile& profile ) const
  {
      std::string out;
      const auto video_tracks = utils::get_media_tracks( *media, VLC_ML_TRACK_TYPE_VIDEO );

      assert( !profile.vcodecs_fallbacks.empty() );
      if ( !video_tracks.empty() )
      {

          video_format_t fmt;
          video_format_Init( &fmt, VLC_CODEC_I420 );

          const vlc_ml_media_track_t& vtrack = video_tracks[0];
          const int sarnum = std::max(1u, vtrack.v.i_sarNum);
          const int sarden = std::max(1u, vtrack.v.i_sarDen);
          video_format_Setup( &fmt, VLC_CODEC_I420, vtrack.v.i_width, vtrack.v.i_height,
                              vtrack.v.i_width, vtrack.v.i_height, sarnum, sarden );

          fmt.i_frame_rate = vtrack.v.i_fpsNum;
          fmt.i_frame_rate_base = vtrack.v.i_fpsDen;

          try
          {
              vlc_fourcc_t codec = 0;
              out = vlc_sout_renderer_GetVcodecOption( VLC_OBJECT( parent ),
                                                        profile.vcodecs_fallbacks, &codec, &fmt,
                                                        profile.conversion_quality );
          }
          catch ( const std::domain_error& e )
          {
              msg_Err( parent, "No video encoder was fitting the transcode chain: %s", e.what() );
          }
          video_format_Clean( &fmt );
      }
      else
          msg_Err( parent, "%s had no video tracks", media->psz_title );
      return out;
  }

  std::string get_audio_transcoding_params( vlc_object_t* ,
                                            const TranscodeProfile& profile ) const
  {
      std::stringstream out;

      if (profile.acodec == 0)
        return "";

      const auto audio_tracks = utils::get_media_tracks( *media, VLC_ML_TRACK_TYPE_AUDIO );
      if ( !audio_tracks.empty() )
      {
          char codec[5] = { 0 };

          vlc_fourcc_to_char( profile.acodec, codec );
          out << "samplerate=48000,channels=2,acodec=" << codec;
      }
      return out.str();
  }
};

//
// Url parsing and FileHandler Factory
//

template <typename MLHelper>
auto get_ml_object( const std::string& token, std::string& extension, vlc_medialibrary_t& ml )
{
    const auto extension_idx = token.find( '.' );
    extension = token.substr( extension_idx + 1 );

    int64_t ml_id;
    try
    {
        ml_id = std::stoll( token.substr( 0, extension_idx ) );
    }
    catch ( const std::invalid_argument& )
    {
        return typename MLHelper::Ptr{ nullptr };
    }
    return MLHelper::get( ml, ml_id );
}

static std::unique_ptr<FileHandler>
parse_media_url( std::stringstream& ss, const intf_sys_t& intf_sys, const ClientProfile& client )
{
    std::string token;
    std::getline( ss, token, '/' );

    const TranscodeProfile* profile = nullptr;
    if ( token != "native" )
    {
        for ( const auto& current_profile : client.transcode_profiles )
        {
            if ( token == current_profile.name )
            {
                profile = &current_profile;
                break;
            }
        }
        if ( profile == nullptr )
            return nullptr;
    }

    std::getline( ss, token );

    std::string extension;
    auto media = get_ml_object<ml::Media>( token, extension, *intf_sys.p_ml );
    if ( media == nullptr )
    {
        return nullptr;
    }

    const auto main_files = utils::get_media_files( *media, VLC_ML_FILE_TYPE_MAIN );
    if ( main_files.empty() )
    {
        return nullptr;
    }
    const vlc_ml_file_t& main_file = main_files.front();

    if ( profile )
    {
        auto mime_type = utils::get_mimetype( media->i_type, profile->mux.file_extension );
        return std::make_unique<TrandscodedMediaFileHandler>( std::move( mime_type ), *profile,
                                                              std::move( media ) );
    }

    auto mime_type = utils::get_mimetype( media->i_type, extension );
    auto ret = std::make_unique<MLFileHandler>(
        MLFile{ main_file.psz_mrl, main_file.i_size, main_file.i_last_modification_date },
        std::move( mime_type ) );
    return ret;
}

static std::unique_ptr<FileHandler> parse_thumbnail_url( std::stringstream& ss,
                                                         const intf_sys_t& intf_sys )
{
    std::string token;
    std::getline( ss, token, '/' );
    vlc_ml_thumbnail_size_t size;
    if ( token == "small" )
        size = VLC_ML_THUMBNAIL_SMALL;
    else if ( token == "banner" )
        size = VLC_ML_THUMBNAIL_BANNER;
    else
        return nullptr;

    std::getline( ss, token, '/' );
    std::string extension;
    std::string mrl;
    if ( token == "media" )
    {
        std::getline( ss, token );
        const auto media = get_ml_object<ml::Media>( token, extension, *intf_sys.p_ml );
        if ( media && media->thumbnails[size].i_status == VLC_ML_THUMBNAIL_STATUS_AVAILABLE )
            mrl = media->thumbnails[size].psz_mrl;
    }
    else if ( token == "album" )
    {
        std::getline( ss, token );
        const auto album = get_ml_object<ml::Album>( token, extension, *intf_sys.p_ml );
        if ( album && album->thumbnails[size].i_status == VLC_ML_THUMBNAIL_STATUS_AVAILABLE )
            mrl = album->thumbnails[size].psz_mrl;
    }

    if ( mrl.empty() )
    {
        return nullptr;
    }

    return std::make_unique<MLFileHandler>( MLFile{ mrl, -1, 0 },
                                            utils::MimeType{ "image", extension } );
}

std::unique_ptr<FileHandler>
parse_url( const char* url, const ClientProfile& client, const intf_sys_t& intf_sys )
{
    std::stringstream ss( url );

    std::string token;
    std::getline( ss, token, '/' );
    if ( !token.empty() )
        return nullptr;

    std::getline( ss, token, '/' );
    if ( token == "media" )
        return parse_media_url( ss, intf_sys, client );
    else if ( token == "thumbnail" )
        return parse_thumbnail_url( ss, intf_sys );
    return nullptr;
}
