#pragma once

#include "Object.hpp"

namespace cds
{
class Container : public Object
{
    protected:
    size_t child_count;

    public:
    struct BrowseParams
    {
        unsigned offset;
        unsigned requested;
    };

    struct BrowseStats
    {
        size_t result_count;
        size_t total_matches;
    };

    Container( const int64_t id,
               const int64_t parent_id,
               const char* name,
               size_t child_count ) noexcept
        : Object( id, parent_id, name, Object::Type::Container )
        , child_count( child_count )
    {
    }

    virtual void dump_metadata( xml::Element& dest, const Object::ExtraId& ) const override
    {
        dest.set_attribute( "childCount", std::to_string( child_count ).c_str() );

        xml::Document& doc = dest.owner;
        dest.add_child(
            doc.create_element( "upnp:class", doc.create_text_node( "object.container" ) ) );
    }

    virtual BrowseStats
    browse_direct_children( xml::Element&, BrowseParams, const Object::ExtraId& ) const = 0;
};
} // namespace cds
