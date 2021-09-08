/*****************************************************************************
 * test/cds.cpp
 *****************************************************************************
 * Copyright © 2019 VLC authors and VideoLAN
 *
 * Authors: Alaric Senat <dev.asenat@posteo.net>
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
#include "../cds/cds.hpp"

#include "cxx_test_helper.hpp"

TEST(ParseIdSimple)
{
    using namespace cds;

    unsigned id;
    Object::ExtraId extra;

    std::tie(id, extra) = parse_id("0");
    ASSERT_EQ(id, 0);
    ASSERT_FALSE(extra.has_value());

    std::tie(id, extra) = parse_id("42");
    ASSERT_EQ(id, 42);
    ASSERT_FALSE(extra.has_value());

    std::tie(id, extra) = parse_id("33:-1");
    ASSERT_EQ(id, 33);
    ASSERT_TRUE(extra.has_value());
    ASSERT_EQ(extra.value().ml_id, -1);

    std::tie(id, extra) = parse_id("0:2222");
    ASSERT_EQ(id, 0);
    ASSERT_TRUE(extra.has_value());
    ASSERT_EQ(extra.value().ml_id, 2222);
}

TEST(ParseIdParent)
{
    using namespace cds;

    unsigned id;
    Object::ExtraId extra;

    std::tie(id, extra) = parse_id("0:3(-1)");
    ASSERT_EQ(id, 0);
    ASSERT_TRUE(extra.has_value());
    ASSERT_EQ(extra.value().ml_id, 3);
    ASSERT_EQ(extra.value().parent, "-1");

    std::tie(id, extra) = parse_id("1:3(24:2)");
    ASSERT_EQ(id, 1);
    ASSERT_TRUE(extra.has_value());
    ASSERT_EQ(extra.value().ml_id, 3);
    ASSERT_EQ(extra.value().parent, "24:2");

    std::tie(id, extra) = parse_id("3:88(0:4(2:34))");
    ASSERT_EQ(id, 3);
    ASSERT_TRUE(extra.has_value());
    ASSERT_EQ(extra.value().ml_id, 88);
    ASSERT_EQ(extra.value().parent, "0:4(2:34)");

    std::tie(id, extra) = parse_id("3:88(0:4(2:34(0:0)))");
    ASSERT_EQ(id, 3);
    ASSERT_TRUE(extra.has_value());
    ASSERT_EQ(extra.value().ml_id, 88);
    ASSERT_EQ(extra.value().parent, "0:4(2:34(0:0))");
}

TEST(ParseIdErrorCases)
{
    using namespace cds;

    unsigned id;
    Object::ExtraId extra;
    ASSERT_THROW(parse_id(""), std::invalid_argument);
    ASSERT_THROW(parse_id(":"), std::invalid_argument);
    ASSERT_THROW(parse_id("text"), std::invalid_argument);

    std::tie(id, extra) = parse_id("0:");
    ASSERT_EQ(id, 0);
    ASSERT_FALSE(extra.has_value());

    std::tie(id, extra) = parse_id("23:(");
    ASSERT_EQ(id, 23);
    ASSERT_FALSE(extra.has_value());

    std::tie(id, extra) = parse_id("8:()");
    ASSERT_EQ(id, 8);
    ASSERT_FALSE(extra.has_value());

    std::tie(id, extra) = parse_id("22:whatever");
    ASSERT_EQ(id, 22);
    ASSERT_FALSE(extra.has_value());

    std::tie(id, extra) = parse_id("2:(1)");
    ASSERT_EQ(id, 2);
    ASSERT_FALSE(extra.has_value());
}
