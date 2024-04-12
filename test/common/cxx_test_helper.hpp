/*****************************************************************************
 * cxx_test_helper.hpp : Small unit test helper for C++
 *****************************************************************************
 * Copyright © 2024 VLC authors and VideoLAN
 *
 * Authors: Alaric Senat <alaric@videolabs.io>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#pragma once

#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace test
{

class AssertFailure : std::exception
{
  public:
    struct Location
    {
        const char *file;
        unsigned int line;
    };

    template <typename T>
    AssertFailure(const Location &location, const char *description, T &&value)
    {
        std::stringstream ss;
        ss << location.file << ":" << location.line << " Assertion " << description
           << " failed:" << std::endl;
        ss << "Value -> " << std::forward<T>(value);
        err_ = ss.str();
    }

    template <typename Lhs, typename Rhs>
    AssertFailure(const Location &location, const char *description, Lhs &&lhs, Rhs &&rhs)
    {
        std::stringstream ss;
        ss << location.file << ":" << location.line << " Assertion " << description
           << " failed:" << std::endl;
        ss << "Left value  -> " << std::forward<Lhs>(lhs) << std::endl;
        ss << "Right value -> " << std::forward<Rhs>(rhs);
        err_ = ss.str();
    }

    template <typename Lhs, typename Rhs>
    AssertFailure(
        const Location &location, const char *description, double epsilon, Lhs &&lhs, Rhs &&rhs)
    {
        std::stringstream ss;
        ss << location.file << ":" << location.line << " Assertion " << description
           << " failed:" << std::endl;
        ss << "Left value  -> " << std::forward<Lhs>(lhs) << std::endl;
        ss << "Right value -> " << std::forward<Rhs>(rhs) << std::endl;
        ss << "Epsilon     -> " << epsilon;
        err_ = ss.str();
    }

    AssertFailure(const Location &location, const char *error)
    {
        std::stringstream ss;
        ss << location.file << ":" << location.line << " Assertion failed:" << std::endl;
        ss << error << ".";
        err_ = ss.str();
    }

    const char *what() const noexcept override { return err_.c_str(); }

  private:
    std::string err_;
};

struct Logger
{
    bool verbose = true;

    void pass(const char test_name[]) const
    {
        if (verbose)
            std::cout << "[PASS] " << test_name << std::endl;
    }

    void fail(const char test_name[], const char *error) const
    {
        std::cerr << "[FAIL] " << test_name << std::endl << error << std::endl;
    }
};

static Logger log;

using TestFn = std::function<bool()>;

struct TestPool
{
    template <typename Test> struct Register
    {
        explicit Register()
        {
            static_test_pool().emplace_back([] {
                Test test;
                try
                {
                    test.run();
                }
                catch (const test::AssertFailure &e)
                {
                    log.fail(Test::NAME, e.what());
                    return false;
                }
                catch (...)
                {
                    log.fail(Test::NAME, "Unexpected exception, rethrowing it...");
                    std::rethrow_exception(std::current_exception());
                }
                log.pass(Test::NAME);
                return true;
            });
        }
    };

    static bool run_all()
    {
        bool success = true;
        for (const auto &test : static_test_pool())
            success &= test();
        return success;
    }

  private:
    static std::vector<TestFn> &static_test_pool()
    {
        static std::vector<TestFn> test_pool;
        return test_pool;
    }
};

#define BUILD_ASSERT_FAILURE(...) test::AssertFailure({__FILE__, __LINE__}, __VA_ARGS__)

#define ASSERT_TRUE(cond)                                                                          \
    if (!(cond))                                                                                   \
        throw BUILD_ASSERT_FAILURE("\"" #cond "\" == true", "false");
#define ASSERT_FALSE(cond)                                                                         \
    if ((cond))                                                                                    \
        throw BUILD_ASSERT_FAILURE("\"" #cond "\" == false", "true");

#define ASSERT_NULL(cond)                                                                          \
    if ((cond) != nullptr)                                                                         \
        throw BUILD_ASSERT_FAILURE("\"" #cond "\" == nullptr", cond);
#define ASSERT_NON_NULL(cond)                                                                      \
    if ((cond) == nullptr)                                                                         \
        throw BUILD_ASSERT_FAILURE("\"" #cond "\" != nullptr", "nullptr");

#define ASSERT_EQ(lhs, rhs)                                                                        \
    if ((lhs) != (rhs))                                                                            \
        throw BUILD_ASSERT_FAILURE("\"" #lhs "\" == \"" #rhs "\"", (lhs), (rhs));
#define ASSERT_NE(lhs, rhs)                                                                        \
    if ((lhs) == (rhs))                                                                            \
        throw BUILD_ASSERT_FAILURE("\"" #lhs "\" != \"" #rhs "\"", (lhs), (rhs));
#define ASSERT_LT(lhs, rhs)                                                                        \
    if ((lhs) >= (rhs))                                                                            \
        throw BUILD_ASSERT_FAILURE("\"" #lhs "\" < \"" #rhs "\"", (lhs), (rhs));
#define ASSERT_LE(lhs, rhs)                                                                        \
    if ((lhs) > (rhs))                                                                             \
        throw BUILD_ASSERT_FAILURE("\"" #lhs "\" <= \"" #rhs "\"", (lhs), (rhs));
#define ASSERT_GT(lhs, rhs)                                                                        \
    if ((lhs) <= (rhs))                                                                            \
        throw BUILD_ASSERT_FAILURE("\"" #lhs "\" > \"" #rhs "\"", (lhs), (rhs));
#define ASSERT_GE(lhs, rhs)                                                                        \
    if ((lhs) < (rhs))                                                                             \
        throw BUILD_ASSERT_FAILURE("\"" #lhs "\" >= \"" #rhs "\"", (lhs), (rhs));

#define ASSERT_NEAR(lhs, rhs, epsilon)                                                             \
    if (std::abs((lhs) - (rhs)) > (epsilon))                                                       \
        throw BUILD_ASSERT_FAILURE("\"" #lhs "\" == \"" #rhs "\"", epsilon, (lhs), (rhs));

#define ASSERT_THROW(statement, exception_type)                                                    \
    {                                                                                              \
        bool thrown = false;                                                                       \
        try                                                                                        \
        {                                                                                          \
            (statement);                                                                           \
        }                                                                                          \
        catch (const exception_type &)                                                             \
        {                                                                                          \
            thrown = true;                                                                         \
        }                                                                                          \
        catch (...)                                                                                \
        {                                                                                          \
            try                                                                                    \
            {                                                                                      \
                std::rethrow_exception(std::current_exception());                                  \
            }                                                                                      \
            catch (const std::exception &e)                                                        \
            {                                                                                      \
                throw BUILD_ASSERT_FAILURE("`" #statement "` to throw \"" #exception_type "\"",    \
                                           e.what());                                              \
            }                                                                                      \
            catch (...)                                                                            \
            {                                                                                      \
                throw BUILD_ASSERT_FAILURE("Expected `" #statement "` to throw \"" #exception_type \
                                           "\" but it threw an unknown type instead");             \
            }                                                                                      \
        }                                                                                          \
        if (!thrown)                                                                               \
            throw BUILD_ASSERT_FAILURE("`" #statement "` did not throw");                          \
    }

#define ASSERT_ANY_THROW(statement)                                                                \
    {                                                                                              \
        bool thrown = false;                                                                       \
        try                                                                                        \
        {                                                                                          \
            (statement);                                                                           \
        }                                                                                          \
        catch (...)                                                                                \
        {                                                                                          \
            thrown = true;                                                                         \
        }                                                                                          \
        if (!thrown)                                                                               \
            throw BUILD_ASSERT_FAILURE("`" #statement "` did not throw");                          \
    }

#define TEST(name)                                                                                 \
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

    /// Called at fixture initialization (optional).
    ///
    /// This function can throw exception safely (including all the
    /// `ASSERT_*` macros) as opposed to the constructor.
    virtual void init(){};

    /// Called at fixture destruction (optional).
    ///
    /// This function can throw exception safely (including all the
    /// `ASSERT_*` macros) as opposed to the destructor.
    virtual void release(){};
};

#define TEST_F(Fixture, name)                                                                      \
    static_assert(std::is_base_of<test::Test, Fixture>::value,                                     \
                  #Fixture " should derive test::Test");                                           \
    struct test_##name : public Fixture                                                            \
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
