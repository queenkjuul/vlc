#pragma once

#include "../ml.hpp"

#include "FixedContainer.hpp"
#include "Item.hpp"
#include "MLContainer.hpp"
//#include "MLContainerList.hpp"

#include <iostream>
#include <sstream>

namespace cds
{

namespace
{
template <typename T>
opt::Option<T> next_value( std::stringstream& ss, const char delim )
{
    std::string token;
    if ( !std::getline( ss, token, delim ) )
        return opt::nullopt;
    try
    {
        return static_cast<T>( std::stoull( token ) );
    }
    catch ( const std::invalid_argument& )
    {
        return opt::nullopt;
    }
};
template <>
opt::Option<std::string> next_value( std::stringstream& ss, const char delim )
{
    std::string token;
    if ( !std::getline( ss, token, delim ) )
        return opt::nullopt;
    if ( token.empty() )
        return opt::nullopt;

    return token;
};
} // namespace

inline std::tuple<unsigned, Object::ExtraId> parse_id( const std::string& id )
{
    std::stringstream ss( id );
    const auto parsed_id = next_value<unsigned>( ss, ':' );
    assert( parsed_id.has_value() ); // TODO proper error handling
    const auto parsed_ml_id = next_value<int64_t>( ss, '(' );
    const auto parsed_parent = next_value<std::string>( ss, ')' );
    if ( parsed_ml_id )
        return { parsed_id.value(), { { parsed_ml_id.value(), parsed_parent.value_or( "" ) } } };
    return { parsed_id.value(), opt::nullopt };
}

inline auto init_hierarchy( vlc_medialibrary_t& ml, const TranscodeProfileList& transcode_profiles )
{
    static Item media( "10", ml, transcode_profiles );

    static MLContainer<ml::AllVideoList, decltype( media )> all_videos( "9", "8", "All video", ml,
                                                                        media );
    static FixedContainer video( "8", "0", "Video", { all_videos } );

    static MLContainer<ml::AlbumTracksList, decltype( media )> album( "3", "2", nullptr, ml,
                                                                      media );

    static MLContainer<ml::ArtistAlbumList, decltype( album )> artists_albums( "7", "6", nullptr,
                                                                               ml, album );
    static MLContainer<ml::AllArtistsList, decltype( artists_albums )> artists_by_album(
        "6", "5", "By album", ml, artists_albums );

    static FixedContainer artists( "5", "1", "Artists", { artists_by_album } );
    static MLContainer<ml::AllAudioList, decltype( media )> all_audio( "4", "1", "All audio", ml,
                                                                       media );
    static MLContainer<ml::AllAlbumList, decltype( album )> all_albums( "2", "1", "All albums", ml,
                                                                        album );
    static FixedContainer music( "1", "0", "Music", { all_albums, all_audio, artists } );

    static FixedContainer home( "0", "-1", "VLC Media Server", { music, video } );
    return std::array<std::reference_wrapper<const Object>, 11>{
        home,           music, all_albums, album, all_audio, artists, artists_by_album,
        artists_albums, video, all_videos, media };
}
} // namespace cds
