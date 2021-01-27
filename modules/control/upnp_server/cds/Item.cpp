#include "Item.hpp"

#include "../ml.hpp"
#include "../utils.hpp"

#include "../stream_out/renderer_common.hpp"
#include "../stream_out/dlna/dlna.hpp"

#include <sstream>
#include <chrono>
#include <ctime>

namespace cds
{

Item::Item( const int64_t id,
            vlc_medialibrary_t& ml,
            const TranscodeProfileList& transcode_profiles ) noexcept
    : Object( id, -1, nullptr, Object::Type::Item )
    , m_medialib( ml )
    , transcode_profiles( transcode_profiles )
{
}

static bool should_propose_transcode( const vlc_ml_media_t& media,
                              const TranscodeProfile& profile,
                              const std::vector<utils::MediaTrackRef>& v_tracks )
{
    return true;
    if ( media.i_type != VLC_ML_MEDIA_TYPE_VIDEO )
        return false;
    for (const vlc_ml_media_track_t& vtrack : v_tracks) {
      switch(profile.i_conversion_quality) {
      case CONVERSION_QUALITY_HIGH:
      case CONVERSION_QUALITY_MEDIUM:
          return vtrack.v.i_width > 1920;
      default:
          return vtrack.v.i_width > 1280;
      }
    }
}

static void dump_resources( xml::Element& dest,
                            const vlc_ml_media_t& media,
                            const TranscodeProfileList& profiles,
                            const std::string& file_extension)
{


    xml::Document& doc = dest.owner;

    const auto v_tracks = utils::get_media_tracks(media, VLC_ML_TRACK_TYPE_VIDEO);
    const auto main_files = utils::get_media_files(media, VLC_ML_FILE_TYPE_MAIN);

    const std::string url_base = utils::get_server_url();

    const auto make_resource = [&]( const TranscodeProfile* profile ) -> xml::Element {
        const char* profile_name = profile ? profile->psz_name : "native";
        const char* mux = (profile && file_extension != "webm") ? "mkv" : file_extension.c_str();

        const std::string url =
            url_base + "media/" + profile_name + "/" + std::to_string( media.i_id ) + "." + mux;
        auto elem = doc.create_element( "res", doc.create_text_node( url.c_str() ) );

        char time_buf[32];
        time_t seconds_elapsed_timestamp = media.i_duration / 1000 - 3600;
        const auto* time_info = std::localtime( &seconds_elapsed_timestamp );
        std::strftime( time_buf, sizeof( time_buf ), "%H:%M:%S", time_info );

        elem.set_attribute( "duration", time_buf );

        if ( media.i_type == VLC_ML_MEDIA_TYPE_VIDEO )
        {
            std::stringstream resolution;

            if ( profile )
            {
                switch ( profile->i_conversion_quality )
                {
                case CONVERSION_QUALITY_HIGH:
                case CONVERSION_QUALITY_MEDIUM:
                    resolution << "1920x1080";
                    break;
                default:
                    resolution << "1280x720";
                    break;
                }
            }
            else
            {
                assert( v_tracks.size() >= 1 );
                const vlc_ml_media_track_t& vtrack = v_tracks[0];

                resolution << vtrack.v.i_width << 'x' << vtrack.v.i_height;
            }
            elem.set_attribute( "resolution", resolution.str().c_str() );
        }

        const auto mime_type = utils::get_mimetype( media.i_type, mux );
        const auto protocol_info =
            std::string( "http-get:*:" ) + mime_type.combine() + ":" +
            utils::get_dlna_extra_protocol_info( mime_type.file_type, profile != nullptr );

        elem.set_attribute( "protocolInfo", protocol_info.c_str() );

        if ( !profile )
        {
            assert( main_files.size() >= 1 );
            elem.set_attribute( "size", std::to_string( main_files[0].get().i_size ).c_str() );
        }


        return elem;
    };

    // Native resource (no transcode)
    dest.add_child( make_resource( nullptr ) );

    // Transcoded resources
    for (unsigned i = 0; i < profiles.u_size; ++i)
    {
        const auto* profile = profiles.p_profiles + i;
        if ( should_propose_transcode( media, *profile, v_tracks ) )
            dest.add_child( make_resource( profile ) );
    }

    // Thumbnails
    for (int i= 0 ; i < VLC_ML_THUMBNAIL_SIZE_COUNT; ++i) {
        const auto& thumbnail = media.thumbnails[i];
        if ( thumbnail.i_status != VLC_ML_THUMBNAIL_STATUS_AVAILABLE )
            continue;
        const auto thumbnail_extension = utils::file_extension( std::string( thumbnail.psz_mrl ) );
        const auto url = utils::thumbnail_url( media, static_cast<vlc_ml_thumbnail_size_t>( i ) );
        auto elem = doc.create_element( "res", doc.create_text_node( url.c_str() ) );

        const auto dlna_extra = utils::get_dlna_extra_protocol_info( thumbnail_extension, false );
        const auto protocol_info =
            std::string( "http-get:*:image/" ) + thumbnail_extension + ":" + dlna_extra;
        elem.set_attribute( "protocolInfo", protocol_info.c_str() );
        
        dest.add_child(std::move(elem));
    }
}

void Item::dump_mlobject_metadata( xml::Element& dest,
                                   const vlc_ml_media_t& media,
                                   vlc_medialibrary_t &ml,
                                   const TranscodeProfileList& transcode_profiles ) noexcept
{

    assert(media.p_files->i_nb_items >= 1);
    const vlc_ml_file_t& file = media.p_files->p_items[0];

    const std::string file_extension = utils::file_extension(file.psz_mrl);

    const char* object_class = nullptr;
    if ( media.i_type == VLC_ML_MEDIA_TYPE_AUDIO )
    {
        object_class = "object.item.audioItem";
    }
    else if ( media.i_type == VLC_ML_MEDIA_TYPE_VIDEO )
    {
        object_class = "object.item.videoItem";
    }
    assert(object_class != nullptr);

    const std::string date = std::to_string( media.i_year ) + "-01-01";

    xml::Document& doc = dest.owner;

    dest.add_children( doc.create_element( "upnp:class", doc.create_text_node( object_class ) ),
                       doc.create_element( "dc:title", doc.create_text_node( media.psz_title ) ),
                       doc.create_element( "dc:date", doc.create_text_node( date.c_str() ) ));
    switch ( media.i_subtype )
    {
    case VLC_ML_MEDIA_SUBTYPE_ALBUMTRACK:
    {
        const auto album = ml::Album::get( ml, media.album_track.i_album_id );
        if ( album != nullptr )
        {
            const auto album_thumbnail_url = utils::album_thumbnail_url( *album );
            dest.add_children(
                doc.create_element( "upnp:album", doc.create_text_node( album->psz_title ) ),
                doc.create_element( "upnp:artist", doc.create_text_node( album->psz_artist ) ),
                doc.create_element( "upnp:albumArtURI",
                                    doc.create_text_node( album_thumbnail_url.c_str() ) ) );
        }
        break;
    }
    default:
        break;
    }

    dump_resources(dest, media, transcode_profiles, file_extension);
}

void Item::dump_metadata( xml::Element& dest, const Object::ExtraId& extra_id ) const
{
    assert( extra_id.has_value() );
    const auto media = ml::Media::get( m_medialib, extra_id->ml_id );

    dump_mlobject_metadata( dest, *media.get(), m_medialib, transcode_profiles );
}
} // namespace cds
