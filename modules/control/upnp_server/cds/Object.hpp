#pragma once

#include "../Option.hpp"
#include "../xml/xml_wrapper.hpp"

namespace cds
{
struct Object
{
    enum class Type
    {
        Item,
        Container
    };

    struct ExtraIdData
    {
        int64_t ml_id;
        std::string parent;
    };

    using ExtraId = opt::Option<ExtraIdData>;

    const char* id;
    const char* parent_id;
    const char* name;
    const Type type;

    Object( const char* id, const char* parent_id, const char* name, const Type type ) noexcept
        : id( id )
        , parent_id( parent_id )
        , name( name )
        , type( type )
    {
    }

    xml::Element browse_metadata( xml::Document& doc, const ExtraId& extra_id ) const
    {
        auto ret = create_object_element( doc, extra_id );

        dump_metadata( ret, extra_id );

        return ret;
    }

    xml::Element create_object_element( xml::Document& doc, const ExtraId& extra_id ) const
    {
        auto ret = doc.create_element( type == Type::Item ? "item" : "container" );

        assert( ( extra_id.has_value() && !extra_id->parent.empty() ) || parent_id );

        if ( extra_id.has_value() )
        {

            ret.set_attribute( "id", get_dynamic_id( extra_id ).c_str() );
            ret.set_attribute( "parentID", extra_id->parent.c_str() );
        }
        else
        {
            ret.set_attribute( "id", id );
            assert( parent_id );
            ret.set_attribute( "parentID", parent_id );
        }

        if ( name )
        {
            ret.add_child( doc.create_element( "dc:title", doc.create_text_node( name ) ) );
        }
        ret.set_attribute( "restricted", "1" );
        return ret;
    }

    protected:
    std::string get_dynamic_id( const ExtraId& extra_id ) const noexcept
    {
        if ( !extra_id.has_value() )
            return std::string( id );
        const std::string ml_id_str = std::to_string( extra_id->ml_id );
        if (!extra_id->parent.empty())
          return std::string( id ) + ':' + ml_id_str + '(' + extra_id->parent + ')';
        return std::string( id ) + ':' + ml_id_str;
    }
    virtual void dump_metadata( xml::Element& dest, const ExtraId& extra_id ) const = 0;
};
} // namespace cds
