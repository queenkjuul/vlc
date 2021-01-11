#include "FixedContainer.hpp"

#include <algorithm>

namespace cds
{

FixedContainer::FixedContainer( const char* id,
                                const char* parent_id,
                                const char* name,
                                std::initializer_list<ObjRef> children ) noexcept
    : Container( id, parent_id, name, children.size() )
    , children( children )
{
}

Container::BrowseStats FixedContainer::browse_direct_children( xml::Element& dest,
                                                               BrowseParams params,
                                                               const Object::ExtraId& extra ) const
{
    assert( child_count == children.size() );
    params.requested =
        std::min( static_cast<size_t>( params.offset ) + params.requested, children.size() );

    unsigned i = 0;
    for ( ; i + params.offset < params.requested; ++i )
    {
        const Object& child = children.at( i + params.offset );
        dest.add_child(child.browse_metadata( dest.owner, extra ));
    }
    return { i, child_count };
}
} // namespace cds
