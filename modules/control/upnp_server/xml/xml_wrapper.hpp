#pragma once

#include <upnp/ixml.h>

#include <cassert>

#include <memory>

/// Simple C++ wrapper around libixml xml library.
namespace xml
{

struct Document;

struct Node
{
    using Ptr = std::unique_ptr<IXML_Node, decltype( &ixmlNode_free )>;
    Ptr ptr;
};

struct Element
{
    using Ptr = std::unique_ptr<IXML_Element, decltype( &ixmlElement_free )>;

    Ptr ptr;
    Document& owner;

    void set_attribute( const char* name, const char* value )
    {
        assert( ptr != nullptr );
        ixmlElement_setAttribute( ptr.get(), name, value );
    }

    void add_child( Node child )
    {
        assert( ptr != nullptr );
        assert( child.ptr != nullptr );
        ixmlNode_appendChild( &ptr->n, child.ptr.release() );
    }

    void add_child( Element child )
    {
        assert( ptr != nullptr );
        assert( child.ptr != nullptr );
        ixmlNode_appendChild( &ptr->n, &child.ptr.release()->n );
    }

    template <typename Child>
    void add_children( Child&& child )
    {
        if ( child.ptr != nullptr )
            add_child( std::move( child ) );
    }

    template <typename First, typename... Child>
    void add_children( First&& first, Child&&... children )
    {
        add_child( std::move( first ) );
        add_children( std::move( children )... );
    }

    private:
    void add_child() {}
};

struct Document
{
    using Ptr = std::unique_ptr<IXML_Document, decltype( &ixmlDocument_free )>;
    Ptr ptr;

    Document() : ptr{ ixmlDocument_createDocument(), &ixmlDocument_free } {}
    Document( Ptr ) = delete;
    Document( Ptr&& ) = delete;

    Node create_text_node( const char* text )
    {
        return Node{ Node::Ptr{ ixmlDocument_createTextNode( ptr.get(), text ), ixmlNode_free } };
    }

    Element create_element( const char* name )
    {
        return Element{
            Element::Ptr{ ixmlDocument_createElement( ptr.get(), name ), &ixmlElement_free },
            *this };
    }

    template <typename... Children>
    Element create_element( const char* name, Children&&... children )
    {
        Element ret = create_element( name );
        ret.add_children( children... );
        return ret;
    }

    void set_entry( Element&& entry ) { ixmlNode_appendChild( &ptr->n, &entry.ptr.release()->n ); }

    using WrappedDOMString = std::unique_ptr<char, decltype( &ixmlFreeDOMString )>;

    WrappedDOMString to_wrapped_cstr() const
    {
        return WrappedDOMString{ ixmlDocumenttoString( ptr.get() ), &ixmlFreeDOMString };
    }
};

} // namespace xml
