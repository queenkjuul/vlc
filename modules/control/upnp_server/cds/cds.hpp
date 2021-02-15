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

std::tuple<unsigned, Object::ExtraId> parse_id( const std::string& id );

std::vector<std::unique_ptr<Object>>
init_hierarchy( vlc_medialibrary_t& ml, const TranscodeProfileList& transcode_profiles );

} // namespace cds
