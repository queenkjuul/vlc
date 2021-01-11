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

template <typename MLHelper>
auto& add_ml_container( std::vector<std::unique_ptr<Object>>& hierarchy,
                        const char* name,
                        vlc_medialibrary_t& ml,
                        const TranscodeProfileList& profiles,
                        const Object& child )
{
    const int64_t id = hierarchy.size();
    hierarchy.push_back( std::make_unique<MLContainer<MLHelper>>( id, -1, name,ml, profiles, child ));
    return static_cast<Object&>( *hierarchy.back() );
}

inline auto init_hierarchy( vlc_medialibrary_t& ml, const TranscodeProfileList& transcode_profiles )
{
  std::vector<std::unique_ptr<Object>> hierarchy;

  const auto add_fixed_container =
      [&]( const char* name, std::initializer_list<FixedContainer::ObjRef> children ) -> Object& {
      const int64_t id = hierarchy.size();
      for ( Object& child : children )
          child.parent_id = id;
      auto up = std::make_unique<FixedContainer>( id, -1, name, children );
      hierarchy.emplace_back( std::move( up ) );
      return *hierarchy.back();
  };


  const auto add_ml_container = [&]( auto MLHelper, const char* name, Object& child) -> Object& {
      const int64_t id = hierarchy.size();
      child.parent_id = id;
      hierarchy.push_back( std::make_unique<MLContainer<decltype( MLHelper )>>(
          id, -1, name, ml, transcode_profiles, child ) );
      return static_cast<Object&>( *hierarchy.back() );
  };

  hierarchy.push_back( std::make_unique<FixedContainer>( 0, -1, "Home" ) );
  hierarchy.push_back( std::make_unique<Item>( 1, ml, transcode_profiles ) );

  const auto& item = static_cast<const Item&>( *hierarchy[1] );

  const auto add_ml_media_container = [&]( auto MLHelper, const char* name) -> Object& {
      const int64_t id = hierarchy.size();
      hierarchy.push_back( std::make_unique<MLContainer<decltype( MLHelper )>>(
          id, -1, name, ml, transcode_profiles, item ) );
      return static_cast<Object&>( *hierarchy.back() );
  };

  static_cast<FixedContainer&>( *hierarchy[0] ).add_children( {
      add_fixed_container( "Video", {
          add_ml_media_container( ml::AllVideoList{}, "All Video" ),
          add_ml_container( ml::ShowsList{}, "Shows",
              add_ml_media_container( ml::ShowsMediaList{}, nullptr ))
      } ),
      add_fixed_container( "Music", {
          add_ml_media_container( ml::AllAudioList{}, "All Audio" ),
          add_ml_container( ml::AllAlbumList{}, "Albums",
                            add_ml_media_container( ml::AlbumTracksList{}, nullptr ) ),
          add_fixed_container( "Artists", {
              add_ml_container( ml::AllArtistsList{}, "By Tracks",
                    add_ml_media_container(ml::ArtistTracksList{}, nullptr ) ),
              add_ml_container( ml::AllArtistsList{}, "By Albums",
                    add_ml_container(ml::ArtistAlbumList{}, nullptr,
                          add_ml_media_container( ml::AlbumTracksList{}, nullptr ) ) )
          })
      } ),
      add_ml_container( ml::PlaylistsList{}, "Playlists",
          add_ml_media_container(ml::PlaylistMediaList{}, nullptr ) )
  } );

  return hierarchy;
}
} // namespace cds
