#include "FileHandler.hpp"

#include <vlc_common.h>

#include <vlc_addons.h>
#include <vlc_cxx_helpers.hpp>
#include <vlc_interface.h>
#include <vlc_player.h>
#include <vlc_rand.h>
#include <vlc_stream_extractor.h>
#include <vlc_url.h>

#include <upnp/ExtraHeaders.h>

#include "stream_out/renderer_common.hpp"

// Convenient cpp replacement of vlc_ml_file_t to not have to deal with allocation
struct MLFile
{
    std::string mrl;
    int64_t size;
    time_t last_modification;
};

static void
add_dlna_headers( UpnpListHead* head, const std::string& file_type, bool transcoding ) noexcept;


struct MLFileHandler : FileHandler
{
    MLFile file;
    utils::MimeType mime_type;

    //TODO up
    stream_t* stream = nullptr;

    MLFileHandler( MLFile&& file, utils::MimeType&& mime_type )
        : file( std::move( file ) )
        , mime_type( std::move( mime_type ) )
    {
    }

    bool get_info( UpnpFileInfo& info ) const noexcept final
    {
        UpnpFileInfo_set_ContentType( &info, mime_type.combine().c_str() );
        UpnpFileInfo_set_FileLength( &info, file.size );
        UpnpFileInfo_set_LastModified( &info, file.last_modification );

        // const_cast is expected by the design of the upnp api...
        auto* head = const_cast<UpnpListHead*>( UpnpFileInfo_get_ExtraHeadersList( &info ) );
        add_dlna_headers(head, mime_type.file_type, false);
        return true;
    }

    bool open( intf_thread_t* parent ) noexcept final
    {
        stream = vlc_stream_NewMRL( VLC_OBJECT(parent), file.mrl.c_str() );
        return stream != nullptr;
    }

    size_t read( uint8_t buffer[], size_t buffer_len ) noexcept final
    {
        return vlc_stream_Read( stream, buffer, buffer_len );
    }

    bool seek( SeekType type, off_t offset ) noexcept final
    {
        uint64_t real_offset;
        switch ( type )
        {
        case SeekType::Current:
            real_offset = vlc_stream_Tell( stream ) + offset;
            break;
        case SeekType::End:
            if ( vlc_stream_GetSize( stream, &real_offset ) != VLC_SUCCESS )
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

        return vlc_stream_Seek( stream, real_offset ) == 0;
    }

    ~MLFileHandler() { vlc_stream_Delete( stream ); }
};

static const char DEFAULT_MUXER[] = "avformat{mux=matroska,options={live=1},reset-ts}";
static const char DEFAULT_MUXER_WEBM[] = "avformat{mux=webm,options={live=1},reset-ts}";

struct TrandscodedMediaFileHandler : FileHandler {
    utils::MimeType mime_type;
    const TranscodeProfile& profile;

    ml::Media::Ptr media;

    struct VLCObject {
      static void release(VLCObject *obj) {
        vlc_object_t* vlc_object = reinterpret_cast<vlc_object_t*>(obj);
        vlc_player_Delete(obj->player);
        auto* fifo =
            static_cast<std::shared_ptr<Fifo>*>( var_GetAddress( vlc_object, Fifo::VAR_NAME ) );
        delete fifo;
        var_SetAddress(vlc_object, Fifo::VAR_NAME, nullptr);
        vlc_object_release(vlc_object);
      }
      using Ptr = std::unique_ptr<VLCObject, decltype(&release)>;

      vlc_object_t obj;
      vlc_player_t *player;
    };
    VLCObject::Ptr obj;
    std::shared_ptr<Fifo> fifo = std::make_shared<Fifo>();

    TrandscodedMediaFileHandler( utils::MimeType&& mime,
                                 const TranscodeProfile& profile,
                                 decltype(media)&& media )
        : mime_type( std::move( mime ) )
        , profile( profile )
        , media( std::move(media) )
        , obj( nullptr, &VLCObject::release )
    {
    }

    bool get_info( UpnpFileInfo& info ) const noexcept final
    {
        assert(media != nullptr);
        const auto main_files = utils::get_media_files(*media, VLC_ML_FILE_TYPE_MAIN);
        if (main_files.empty())
          return false;

        UpnpFileInfo_set_ContentType( &info, mime_type.combine().c_str() );
        UpnpFileInfo_set_LastModified( &info, main_files.front().get().i_last_modification_date );

        // We can't predict the media size before transcoding.
        UpnpFileInfo_set_FileLength( &info, -1 );
        
        // const_cast is expected by the design of the upnp api...
        auto* head = const_cast<UpnpListHead*>( UpnpFileInfo_get_ExtraHeadersList( &info ) );
        add_dlna_headers(head, mime_type.file_type, true);
        return true;
    }

    bool open( intf_thread_t* parent ) noexcept final
    {
        obj = VLCObject::Ptr{
            static_cast<VLCObject*>( vlc_object_create( parent, sizeof( VLCObject ) ) ),
            &VLCObject::release };
        
        obj->player = vlc_player_New(&obj->obj, VLC_PLAYER_LOCK_NORMAL, nullptr, nullptr );
        if (obj->player == nullptr) {
          return false;
        }

        const auto main_files = utils::get_media_files(*media, VLC_ML_FILE_TYPE_MAIN);
        if (main_files.empty())
          return false;
        const vlc_ml_file_t &main_file = main_files.front();

        auto item =
            vlc::wrap_cptr( vlc_ml_get_input_item_by_mrl( parent->p_sys->p_ml, main_file.psz_mrl ),
                            &input_item_Release );
        if (item == nullptr) {
          return false;
        }

        const auto video_tracks = utils::get_media_tracks( *media, VLC_ML_TRACK_TYPE_VIDEO );
        video_format_t fmt;
        video_format_Init( &fmt, VLC_CODEC_I420 );
        if ( video_tracks.empty() )
        {
          return false;
        }
        //TODO should be a runtime check, not sure about the correct behaviour tho.
        assert( video_tracks.size() == 1 );

        const vlc_ml_media_track_t& vtrack = video_tracks[0];
        video_format_Setup( &fmt, VLC_CODEC_I420, vtrack.v.i_width, vtrack.v.i_height,
                            vtrack.v.i_width, vtrack.v.i_height, 1, 1 );

        vlc_fourcc_t codec = 0;
        std::stringstream transcode_line;
        transcode_line << "sout=#transcode{";
        try
        {
            transcode_line << vlc_sout_renderer_GetVcodecOption(
                VLC_OBJECT( parent ), profile.codecs_fallbacks, &codec,
                &fmt,
                profile.i_conversion_quality ) /*<< "height=1080"*/;
        }
        catch ( const std::domain_error& e )
        {
            msg_Err( parent, "No encoder was fitting the transcode chain: %s", e.what() );
            return false;
        }
        transcode_line << "}";

        const char* mux = mime_type.file_type == "webm" ? DEFAULT_MUXER_WEBM : DEFAULT_MUXER;
        transcode_line << ":std{access=upnp-out,mux=" << mux << "}";

        const auto transcode_line_str = transcode_line.str();
        int res =
            input_item_AddOption( item.get(), transcode_line_str.c_str(), VLC_INPUT_OPTION_TRUSTED );

        if ( res != VLC_SUCCESS )
        {
            msg_Err( parent, "Can't append transcode chain to the player" );
            return false;
        }

        msg_Info( parent, "Transcoding '%s' with profile '%s'", media->psz_title,
                  profile.psz_name );
        msg_Dbg( parent, "Transcode chain: %s", transcode_line_str.c_str() );

        var_Create( &obj->obj, Fifo::VAR_NAME, VLC_VAR_ADDRESS );
        var_SetAddress( &obj->obj, Fifo::VAR_NAME, new decltype(fifo)(fifo) );

        vlc_player_Lock( obj->player );
        const bool ret = vlc_player_SetCurrentMedia( obj->player, item.get() ) == VLC_SUCCESS;
        vlc_player_Start( obj->player );
        vlc_player_Unlock( obj->player );
        return ret;
    }

    size_t read( uint8_t buffer[], size_t buffer_len ) noexcept final
    {
        return fifo->read( buffer, buffer_len );
    }

    // Byte based seeking isn't supported in transcoding.
    bool seek( SeekType, off_t ) noexcept final { return false; }
};

template <typename MLHelper>
auto get_ml_object(const std::string &token, std::string & extension, vlc_medialibrary_t& ml)
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
        return typename MLHelper::Ptr{nullptr};
    }
    return MLHelper::get(ml, ml_id);
}

static std::unique_ptr<FileHandler> parse_media_url( std::stringstream& ss,
                                                         const intf_sys_t& intf_sys )
{
    std::string token;
    std::getline( ss, token, '/' );

    const TranscodeProfile *profile = nullptr;
    if ( token != "native" )
    {
        for ( unsigned i = 0; i < intf_sys.transcode_profiles.u_size; ++i )
        {
            const TranscodeProfile& current_profile = intf_sys.transcode_profiles.p_profiles[i];
            if ( token == current_profile.psz_name )
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
    auto media = get_ml_object<ml::Media>(token, extension, *intf_sys.p_ml);
    if ( media == nullptr )
    {
        return nullptr;
    }

    const auto main_files = utils::get_media_files(*media, VLC_ML_FILE_TYPE_MAIN);
    if ( main_files.empty() )
    {
        return nullptr;
    }
    const vlc_ml_file_t & main_file = main_files.front();


    if (profile)
    {
        auto mime_type =
            utils::get_mimetype( media->i_type, extension != "webm" ? "mkv" : extension );
        return std::make_unique<TrandscodedMediaFileHandler>( std::move( mime_type ), *profile,
                                                              std::move( media ) );
    }

    auto mime_type = utils::get_mimetype( media->i_type, extension );
    return std::make_unique<MLFileHandler>(
        MLFile{ main_file.psz_mrl, main_file.i_size, main_file.i_last_modification_date },
        std::move(mime_type) );
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

    if (mrl.empty())
    {
        return nullptr;
    }

    return std::make_unique<MLFileHandler>( MLFile{ mrl, -1, 0 },
                                            utils::MimeType{ "image", extension } );
}

static std::unique_ptr<FileHandler> parse_subtitle_url( std::stringstream& ss,
                                                         const intf_sys_t& intf_sys )
{
    std::string token;
    std::string extension;
    std::string mrl;

    std::getline( ss, token );
    const auto media = get_ml_object<ml::Media>( token, extension, *intf_sys.p_ml );
    if (media == nullptr)
    {
      return nullptr;
    }

    const auto subtitles = utils::get_media_files(*media, VLC_ML_FILE_TYPE_SUBTITLE);
    if (subtitles.empty())
    {
      return nullptr;
    }

    const vlc_ml_file_t& sub = subtitles.front();
    return std::make_unique<MLFileHandler>( MLFile{ sub.psz_mrl, sub.i_size, sub.i_last_modification_date },
                                            utils::MimeType{ "text", extension } );
}

std::unique_ptr<FileHandler> parse_url( const char* url, const intf_sys_t& intf_sys ) noexcept
{
    std::stringstream ss( url );

    std::string token;
    std::getline( ss, token, '/' );
    if ( !token.empty() )
        return nullptr;

    std::getline( ss, token, '/' );
    if ( token == "media" )
        return parse_media_url( ss, intf_sys );
    else if ( token == "thumbnail" )
        return parse_thumbnail_url( ss, intf_sys );
    else if ( token == "subtitle" )
        return parse_subtitle_url( ss, intf_sys );
    return nullptr;
}

static void
add_dlna_headers( UpnpListHead* head, const std::string& file_type, bool transcoding ) noexcept
{
    {
        UpnpExtraHeaders* h = UpnpExtraHeaders_new();
        UpnpExtraHeaders_set_resp( h, "transferMode.dlna.org: Streaming" );
        UpnpListInsert( head, UpnpListEnd( head ),
                        const_cast<UpnpListHead*>( UpnpExtraHeaders_get_node( h ) ) );
    }
    {
        UpnpExtraHeaders* h = UpnpExtraHeaders_new();
        const auto content_feature = utils::get_dlna_extra_protocol_info( file_type, transcoding );
        UpnpExtraHeaders_set_resp(
            h, std::string( "contentFeatures.dlna.org: " + content_feature ).c_str() );
        UpnpListInsert( head, UpnpListEnd( head ),
                        const_cast<UpnpListHead*>( UpnpExtraHeaders_get_node( h ) ) );
    }
}
