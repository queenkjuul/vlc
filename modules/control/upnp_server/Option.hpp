#pragma once

#include <cassert>
#include <exception>
#include <string>
#include <type_traits>

#include <iostream>

// This is a straightforward and naive implementation of std::optional which is
// only available in c++17.
// TODO remplace all Option instances by std::optional when we bump to c++17.

namespace opt
{

struct nullopt_t
{
    // explicit ctor to support both "op = {};" and "op = nullopt;"
    constexpr explicit nullopt_t( int ) {}
};

static constexpr nullopt_t nullopt( 0 );

struct bad_optional_access : std::exception
{
    const char* what() const noexcept final { return "Bad optional access"; }
};

template <typename T>
struct Option
{
    constexpr Option( nullopt_t ) noexcept : _has_value( false ) {}
    constexpr Option( T&& value ) noexcept : _has_value( true ), _value( std::forward<T>( value ) )
    {
    }

    constexpr Option() noexcept = default;
    constexpr Option( const Option& other ) noexcept = default;

    constexpr operator bool() const noexcept { return _has_value; }
    constexpr bool has_value() const noexcept { return _has_value; }
    constexpr bool operator==( nullopt_t ) noexcept { return !_has_value; }
    constexpr bool operator!=( nullopt_t ) noexcept { return _has_value; }

    constexpr T& value()
    {
        if ( not _has_value )
            throw bad_optional_access{};
        return _value;
    }

    constexpr const T& value() const
    {
        if ( not _has_value )
            throw bad_optional_access{};
        return _value;
    }

    template <typename U = T>
    constexpr T value_or( U&& default_value ) const noexcept
    {
        return _has_value ? _value : static_cast<T>( std::forward<T>( default_value ) );
    }

    constexpr T* operator->() noexcept { return &_value; }
    constexpr const T* operator->() const noexcept { return &_value; }
    constexpr T& operator*() noexcept { return _value; }
    constexpr const T& operator*() const noexcept { return _value; }

    constexpr Option& operator=( T&& value )
    {
        _value = std::forward<T>( value );
        _has_value = true;
        return *this;
    }

    private:
    bool _has_value = false;
    T _value = {};
};

namespace
{
static_assert( std::is_trivially_copyable<opt::Option<int>>::value, "" );

static_assert( !bool( Option<int>() ), "" );
static_assert( !Option<int>().has_value(), "" );
static_assert( Option<int>() == nullopt, "" );

static_assert( bool( Option<int>( 42 ) ), "" );
static_assert( Option<int>( 42 ).has_value(), "" );
static_assert( Option<int>( 42 ) != nullopt, "" );

static_assert( Option<int>( 42 ).value() == 42, "" );
static_assert( Option<int>( 42 ).value() != 43, "" );

static_assert( Option<int>( 42 ).value_or( 0 ) == 42, "" );
} // namespace
} // namespace opt
