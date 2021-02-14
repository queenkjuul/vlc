#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <type_traits>
#include <vector>

namespace test
{

struct AssertFailure : std::exception
{
    template <typename T>
    AssertFailure( const std::pair<std::string, int>& location, const char* prelude, T&& value )
    {
        std::stringstream ss;
        ss << location.first << ":" << location.second << " Assertion failed" << std::endl;
        ss << prelude << ", actual: " << std::forward<T>( value );
        err = ss.str();
    }

    template <typename Lhs, typename Rhs>
    AssertFailure( const std::pair<std::string, int>& location,
                   const char* prelude,
                   Lhs&& lhs,
                   Rhs&& rhs )
    {
        std::stringstream ss( prelude );
        ss << location.first << ":" << location.second << " Assertion failed" << std::endl;
        ss << prelude << ", actual: " << std::forward<Lhs>( lhs ) << " vs "
           << std::forward<Rhs>( rhs );
        err = ss.str();
    }

    const char* what() const noexcept override { return err.c_str(); }

    private:
    std::string err;
};

struct Logger
{

    enum class State
    {
        Pass,
        Fail
    };

    bool verbose = true;

    void pass( const char test_name[] ) const
    {
        if ( verbose )
        {
            std::cout << "[PASS] " << test_name << std::endl;
        }
    }

    void fail( const char test_name[], const char* error ) const
    {
        std::cerr << "[FAIL] " << test_name << std::endl << error << std::endl;
    }
};

using TestFn = std::function<int( const Logger& )>;
struct TestPool
{
    template <typename Test>
    struct Register
    {
        explicit Register()
        {
            static_test_pool().emplace_back( []( const Logger& log ) {
                try
                {
                    Test().run();
                }
                catch ( const test::AssertFailure& e )
                {
                    log.fail( Test::NAME, e.what() );
                    return false;
                }
                catch ( ... )
                {
                    log.fail( Test::NAME, "Unexpected exception, rethrowing it..." );
                    std::rethrow_exception( std::current_exception() );
                }
                log.pass( Test::NAME );
                return true;
            } );
        }
    };

    static bool run_all_tests( const Logger& logger )
    {
        bool success = 0;
        for ( const auto& test : static_test_pool() )
        {
            success &= test( logger );
        }
        return success;
    }

    private:
    static std::vector<TestFn>& static_test_pool()
    {
        static std::vector<TestFn> test_pool;
        return test_pool;
    }
};

#define BUILD_ASSERT_FAILURE( ... ) test::AssertFailure( { __FILE__, __LINE__ }, __VA_ARGS__ )

#define ASSERT_TRUE( cond )                                                                        \
    if ( !( cond ) )                                                                               \
        throw BUILD_ASSERT_FAILURE( "Expected (" #cond ") to be true", "false" );
#define ASSERT_FALSE( cond )                                                                       \
    if ( ( cond ) )                                                                                \
        throw BUILD_ASSERT_FAILURE( "Expected (" #cond ") to be false", "true" );

#define ASSERT_EQ( lhs, rhs )                                                                      \
    if ( ( lhs ) != ( rhs ) )                                                                      \
        throw BUILD_ASSERT_FAILURE( "Expected (" #lhs ") == (" #rhs ")", ( lhs ), ( rhs ) );
#define ASSERT_NE( lhs, rhs )                                                                      \
    if ( ( lhs ) == ( rhs ) )                                                                      \
        throw BUILD_ASSERT_FAILURE( "Expected (" #lhs ") != (" #rhs ")", ( lhs ), ( rhs ) );
#define ASSERT_LT( lhs, rhs )                                                                      \
    if ( ( lhs ) <= ( rhs ) )                                                                      \
        throw BUILD_ASSERT_FAILURE( "Expected (" #lhs ") < (" #rhs ")", ( lhs ), ( rhs ) );
#define ASSERT_LE( lhs, rhs )                                                                      \
    if ( ( lhs ) > ( rhs ) )                                                                       \
        throw BUILD_ASSERT_FAILURE( "Expected (" #lhs ") <= (" #rhs ")", ( lhs ), ( rhs ) );
#define ASSERT_GT( lhs, rhs )                                                                      \
    if ( ( lhs ) <= ( rhs ) )                                                                      \
        throw BUILD_ASSERT_FAILURE( "Expected (" #lhs ") > (" #rhs ")", ( lhs ), ( rhs ) );
#define ASSERT_GE( lhs, rhs )                                                                      \
    if ( ( lhs ) < ( rhs ) )                                                                       \
        throw BUILD_ASSERT_FAILURE( "Expected (" #lhs ") >= (" #rhs ")", ( lhs ), ( rhs ) );

#define ASSERT_NEAR( lhs, rhs, epsilon )                                                           \
    if ( std::abs( ( lhs ) - ( rhs ) ) > ( epsilon ) )                                             \
        throw BUILD_ASSERT_FAILURE( "Expected abs((" #lhs ") - (" #rhs ")) <= (" #epsilon ")",     \
                                    ( lhs ), ( rhs ) );

#define ASSERT_THROW( statement, exception_type )                                                  \
    {                                                                                              \
        bool throwed = false                                                                       \
        try                                                                                        \
        {                                                                                          \
            ( statement );                                                                         \
        }                                                                                          \
        catch ( const( exception_type )& )                                                         \
        {                                                                                          \
            throwed = true;                                                                        \
        }                                                                                          \
        catch ( ... )                                                                              \
        {                                                                                          \
            throw BUILD_ASSERT_FAILURE( "Expected (" #exception_type                               \
                                        ") to be thrown but caught another" );                     \
        }                                                                                          \
        if ( !throwed )                                                                            \
            throw BUILD_ASSERT_FAILURE( "Did not throw" );                                         \
    }
#define ASSERT_ANY_THROW( statement )                                                              \
    {                                                                                              \
        bool throwed = false                                                                       \
        try                                                                                        \
        {                                                                                          \
            ( statement );                                                                         \
        }                                                                                          \
        catch ( ... )                                                                              \
        {                                                                                          \
            throwed = true;                                                                        \
        }                                                                                          \
        if ( !throwed )                                                                            \
            throw BUILD_ASSERT_FAILURE( "Did not throw" );                                         \
    }
#define ASSERT_NO_THROW( statement )                                                               \
    try                                                                                            \
    {                                                                                              \
        ( statement );                                                                             \
    }                                                                                              \
    catch ( ... )                                                                                  \
    {                                                                                              \
        throw BUILD_ASSERT_FAILURE( "Unexpected exception caught" );                               \
    }

#define TEST( name )                                                                               \
    struct test_##name                                                                             \
    {                                                                                              \
        static constexpr auto NAME = #name;                                                        \
        void run();                                                                                \
    };                                                                                             \
    test::TestPool::Register<test_##name> test_impl_##name;                                        \
    void test_##name::run()

struct Test
{
    virtual ~Test() = default;

    virtual void init(){};
    virtual void release(){};
};

#define TEST_F( Fixture, name )                                                                    \
    static_assert( std::is_base_of<test::Test, Fixture>::value,                                    \
                   #Fixture " should derive test::Test" );                                         \
    struct test_##name : ( Fixture )                                                               \
    {                                                                                              \
        static constexpr auto NAME = #name;                                                        \
        void do_run();                                                                             \
        void run()                                                                                 \
        {                                                                                          \
            init();                                                                                \
            do_run();                                                                              \
            release();                                                                             \
        }                                                                                          \
    };                                                                                             \
    test::TestPool::Register<test_##name> test_impl_##name;                                        \
    void test_##name::do_run()

} // namespace test
