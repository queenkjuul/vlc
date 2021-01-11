#pragma once

#include "Container.hpp"

#include <vector>

namespace cds {
  struct FixedContainer : public Container {
    using ObjRef = std::reference_wrapper<Object>;
    FixedContainer( const int64_t id,
                    const int64_t parent_id,
                    const char* name,
                    std::initializer_list<ObjRef> ) noexcept;
    FixedContainer( const int64_t id,
                    const int64_t parent_id,
                    const char* name) noexcept;

    BrowseStats
    browse_direct_children( xml::Element&, BrowseParams, const Object::ExtraId& ) const final;

    void add_children(std::initializer_list<ObjRef> l) {
      for (Object& child : l) {
        child.parent_id = id;
      }
      children.insert(children.end(), l.begin(), l.end());
      child_count = children.size();
    }
    private:
    std::vector<ObjRef> children;
  };
}
