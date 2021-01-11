#pragma once

#include "Container.hpp"

#include <vector>

namespace cds {
  class FixedContainer : public Container {
    using ObjRef = std::reference_wrapper<const Object>;
    std::vector<ObjRef> children;

    public:
    FixedContainer( const char* id,
                    const char* parent_id,
                    const char* name,
                    std::initializer_list<ObjRef> ) noexcept;

    BrowseStats
    browse_direct_children( xml::Element&, BrowseParams, const Object::ExtraId& ) const final;
  };
}
