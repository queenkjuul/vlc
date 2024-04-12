/*****************************************************************************
 * cxx_test_helper.cpp : C++ test helper sample file
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

#include <common/cxx_test_helper.hpp>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string_view>

TEST(SuccessBehavior)
{
    ASSERT_TRUE(true);
    ASSERT_FALSE(false);

    ASSERT_EQ(42, 42);
    ASSERT_NE(43, 42);

    ASSERT_LT(41, 42);

    ASSERT_LE(41, 42);
    ASSERT_LE(42, 42);

    ASSERT_GT(43, 42);

    ASSERT_GE(43, 42);
    ASSERT_GE(42, 42);

    ASSERT_NEAR(1.1, 1.10001, 0.00001);
    ASSERT_NEAR(1.1, 1.10001, 0.1);

    ASSERT_THROW([] { throw static_cast<int>(6); }(), int)
    ASSERT_ANY_THROW([] { throw 6.; }())
}

TEST(FailureBehavior)
{
#define ASSERT_FAIL_TEST(statement) ASSERT_THROW([] { statement }(), test::AssertFailure)

    ASSERT_FAIL_TEST(ASSERT_TRUE(false));
    ASSERT_FAIL_TEST(ASSERT_FALSE(true));

    using namespace std::string_view_literals;
    ASSERT_FAIL_TEST(ASSERT_EQ("abc"sv, "abcd"sv));
    ASSERT_FAIL_TEST(ASSERT_NE("abcd"sv, "abcd"sv));

    ASSERT_FAIL_TEST(ASSERT_LT(1337, 1337));
    ASSERT_FAIL_TEST(ASSERT_LT(1337, 1336));

    ASSERT_FAIL_TEST(ASSERT_LE(1997, 1996));

    ASSERT_FAIL_TEST(ASSERT_GT(2000, 2000));
    ASSERT_FAIL_TEST(ASSERT_GT(1999, 2000));

    ASSERT_FAIL_TEST(ASSERT_GE(0, 1));

    ASSERT_FAIL_TEST(ASSERT_NEAR(1.1, 1.10001, 0.00000001));

    ASSERT_FAIL_TEST(ASSERT_THROW([] { throw "not an int"; }(), int))
    ASSERT_FAIL_TEST(ASSERT_THROW([] { /* not throwing */ }(), int))

    ASSERT_FAIL_TEST(ASSERT_ANY_THROW([] { /* not throwing */ }()))

#undef ASSERT_FAIL_TEST
}

class FixtureClass : public test::Test
{
  public:
    virtual ~FixtureClass() = default;

    void init() final
    {
        ptr = new (std::nothrow) int64_t;
        ASSERT_NON_NULL(ptr);
    }

    void release() final { delete ptr; }

  protected:
    int64_t *ptr = nullptr;
};

TEST_F(FixtureClass, FixtureTestOne)
{
    *ptr = 0xfa11afe1;
    ASSERT_EQ(*ptr, 0xfa11afe1);
}

TEST_F(FixtureClass, FixtureTestTwo)
{
    *ptr = 0xbeef;
    ASSERT_EQ(*ptr, 0xbeef);
}

int main() { return test::TestPool::run_all() ? EXIT_SUCCESS : EXIT_FAILURE; }
