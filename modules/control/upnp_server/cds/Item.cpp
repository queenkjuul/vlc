#include "Item.hpp"

#include "../ml.hpp"
#include "../utils.hpp"

#include <sstream>
#include <chrono>
#include <ctime>

namespace cds
{

Item::Item( const char* id,
            vlc_medialibrary_t& ml,
            const TranscodeProfileList& transcode_profiles ) noexcept
    : Object( id, nullptr, nullptr, Object::Type::Item )
    , m_medialib( ml )
    , transcode_profiles( transcode_profiles )
{
}

static bool should_transcode( const vlc_ml_media_t& media,
                              const TranscodeProfile& profile,
                              const std::vector<utils::MediaTrackRef>& v_tracks )
{
    if ( media.i_type != profile.i_type )
        return false;
    for (const vlc_ml_media_track_t& vtrack : v_tracks) {
        if ( vtrack.v.i_height > profile.resolution.height ||
             vtrack.v.i_width > profile.resolution.width )
          return true;
    }
    return false;
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
      const char * profile_name = profile ? profile->psz_name : "native";
      const char *mux = profile && profile->mux ? profile->mux : file_extension.c_str();
      const auto mime_type =
          std::string("http-get:*:") + ( media.i_type == VLC_ML_MEDIA_TYPE_AUDIO ? "audio/" : "video/" ) + mux +
          ":*";
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
          assert( v_tracks.size() >= 1 );

          const vlc_ml_media_track_t& vtrack = v_tracks[0];

          const unsigned width = profile ? profile->resolution.width : vtrack.v.i_width;
          const unsigned height = profile ? profile->resolution.height : vtrack.v.i_height;
          const std::string resolution = std::to_string( width ) + "x" + std::to_string( height );

          elem.set_attribute( "resolution", resolution.c_str() );
        }

        elem.set_attribute( "protocolInfo", mime_type.c_str() );

        if ( !profile )
        {
          assert(main_files.size() >= 1);
          elem.set_attribute( "size", std::to_string( main_files[0].get().i_size ).c_str() );
        }

        return elem;
    };

    dest.add_child( make_resource( nullptr ) );

    for (unsigned i = 0; i < profiles.u_size; ++i)
    {
        const auto* profile = profiles.p_profiles + i;
        if ( should_transcode(media, *profile, v_tracks) )
            dest.add_child( make_resource( profile ) );
    }
}

void Item::dump_mlobject_metadata( xml::Element& dest, const vlc_ml_media_t& media ) const noexcept
{

    assert(media.p_files->i_nb_items >= 1);
    const vlc_ml_file_t& file = media.p_files->p_items[0];

    const std::string file_extension = utils::file_extension(file.psz_mrl);

    const char* object_class;
    if ( media.i_type == VLC_ML_MEDIA_TYPE_AUDIO )
    {
        object_class = "object.item.audioItem";
    }
    else if ( media.i_type == VLC_ML_MEDIA_TYPE_VIDEO )
    {
        object_class = "object.item.videoItem";
    }

    const std::string date = std::to_string( media.i_year ) + "-01-01";


    xml::Document& doc = dest.owner;

    dest.add_children( doc.create_element( "upnp:class", doc.create_text_node( object_class ) ),
                       doc.create_element( "dc:title", doc.create_text_node( media.psz_title ) ),
                       doc.create_element( "dc:date", doc.create_text_node( date.c_str() ) ));

    dump_resources(dest, media, transcode_profiles, file_extension);
}

void Item::dump_metadata( xml::Element& dest, const Object::ExtraId& extra_id ) const
{
    assert( extra_id.has_value() );
    const auto media = ml::Media::get( m_medialib, extra_id->ml_id );

    dump_mlobject_metadata( dest, *media.get() );
}
} // namespace cds
