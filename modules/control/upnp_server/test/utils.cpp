/*****************************************************************************
 * test/utils.cpp
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
#include "../utils.hpp"

#include <chrono>

#include "./cxx_test_helper.hpp"

TEST(ParseDlnaNpt)
{
    using namespace utils;
    using namespace std::chrono;

    const auto to_ms = [](auto s) { return duration_cast<milliseconds>(s).count(); };

    // Invalid cases
    ASSERT_EQ(utils::parse_dlna_npt_range(""), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("npt="), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=-"), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=-5"), 0);

    ASSERT_EQ(utils::parse_dlna_npt_range("32:a:2-"), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("abcd"), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("abc=4"), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("55"), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=-2"), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=42"), 0);

    // Time specified in seconds
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=0-"), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=42-"), to_ms(42s));
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=55.32-"), to_ms(55s + 320ms));
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=23.4-"), to_ms(23s + 400ms));
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=443-"), to_ms(443s));
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=175.2346-"), to_ms(175s + 234ms));
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=534.250-"), to_ms(534s + 250ms));

    // Invalid date time
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=a:05:23-"), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=1:w:23-"), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=1:05:b-"), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=1:05:22"), 0);

    // Time specified by date
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=00:00:00-"), 0);
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=12:05:23-"), to_ms(12h + 5min + 23s));
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=1:00:00.5-"), to_ms(1h + 500ms));
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=00:1:00-"), to_ms(1min));
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=00:00:1-"), to_ms(1s));
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=1:2:3-"), to_ms(1h + 2min + 3s));
    ASSERT_EQ(utils::parse_dlna_npt_range("npt=25:1:2-"), to_ms(25h + 1min + 2s));
}
