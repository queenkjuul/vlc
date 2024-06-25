/*****************************************************************************
 * codecs.c:
 *****************************************************************************
 * Copyright (C) 2023 VLC authors and VideoLAN
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>

#include <vlc_memstream.h>
#include <vlc_bits.h>

#include "../codec/hxxx_helper.h"
#include "../packetizer/hxxx_ep3b.h"
#include "codecs.h"

#include <assert.h>
static int FormatAVC1(struct vlc_memstream *ms, const es_format_t *fmt)
{
    /* Parse the h264 constraint flag. */
    uint8_t constraints = 0;
    uint8_t profile = 0;
    uint8_t level = 0;
    struct hxxx_helper hh;
    hxxx_helper_init(&hh, NULL, fmt->i_codec, 0, 0);
    if (hxxx_helper_set_extra(&hh, fmt->p_extra, fmt->i_extra) == VLC_SUCCESS)
    {
        h264_helper_get_constraint_flag(&hh, &constraints);
        hxxx_helper_get_current_profile_level(&hh, &profile, &level);
    }
    hxxx_helper_clean(&hh);

    const int wrote = vlc_memstream_printf(
        ms, "avc1.%02X%02X%02X", profile, constraints, level);
    return (wrote == -1) ? VLC_ENOMEM : VLC_SUCCESS;
}

static int FormatMP4A(struct vlc_memstream *ms, const es_format_t *fmt)
{
    const uint8_t obtyp = (((const char *)fmt->p_extra)[0] & 0xf8) >> 3;
    const int wrote = vlc_memstream_printf(
        ms, "mp4a.40.%01d", (fmt->i_profile == -1) ? 2u : obtyp);
    return (wrote == -1) ? VLC_ENOMEM : VLC_SUCCESS;
}

static int FormatH265(struct vlc_memstream *ms, const es_format_t *fmt)
{
    /* Parse the H.265/HEVC codec information. */

    struct hxxx_helper hh;
    hxxx_helper_init(&hh, NULL, fmt->i_codec, 0, 0);
    hxxx_helper_set_extra(&hh, fmt->p_extra, fmt->i_extra);

    vlc_memstream_puts(ms, "hev1");

    const struct hxxx_helper_nal *hsps = &hh.hevc.sps_list[hh.hevc.i_current_sps];
    struct hxxx_bsfw_ep3b_ctx_s ctx;
    hxxx_bsfw_ep3b_ctx_init( &ctx );

    bs_t bs;
    bs_init_custom(&bs, hsps->b->p_buffer, hsps->b->i_buffer, &hxxx_bsfw_ep3b_callbacks, &ctx);

    /* Jump to profile_tier_level. */
    bs_skip(&bs, 24);

    const uint8_t profile_space = bs_read(&bs, 2);
    if (profile_space == 1)
        vlc_memstream_putc(ms, 'A');
    else if (profile_space == 2)
        vlc_memstream_putc(ms, 'B');
    else if (profile_space == 3)
        vlc_memstream_putc(ms, 'C');

    const uint8_t tier_flag = bs_read1(&bs);
    
    const uint8_t profile_idc = bs_read(&bs, 5);
    vlc_memstream_printf(ms, ".%u", profile_idc);

    uint32_t profile_compatibility_flag = bs_read(&bs, 32);
    //vlc_memstream_printf(ms, ".%X", profile_compatibility_flag);
    uint32_t i, res = 0;
    for (i=0; i<32; i++) {
        res |= profile_compatibility_flag & 1;
        if (i==31) break;
        res <<= 1;
        profile_compatibility_flag >>=1;
    }
    vlc_memstream_printf(ms, ".%X", res);

    vlc_memstream_puts(ms, (tier_flag) ? ".H" : ".L");

    const uint8_t constraints = bs_read(&bs, 8);
    bs_skip(&bs, 40);

    const uint8_t level_idc = bs_read(&bs, 8);
    vlc_memstream_printf(ms, "%u", level_idc);

    vlc_memstream_printf(ms, ".%X", constraints);

    return VLC_SUCCESS;
}

static int FormatWebVTT(struct vlc_memstream *ms)
{
    //const int written = vlc_memstream_puts(ms, "wvtt");
    //return (written <= 0) ? VLC_ENOMEM : VLC_SUCCESS;
    return VLC_SUCCESS;
}

int hls_codec_Format(struct vlc_memstream *ms, const es_format_t *fmt)
{
    switch (fmt->i_codec)
    {
        case VLC_CODEC_H264:
            return FormatAVC1(ms, fmt);
        case VLC_CODEC_MP4A:
            return FormatMP4A(ms, fmt);
        case VLC_CODEC_TEXT:
        case VLC_CODEC_WEBVTT:
        case VLC_CODEC_SUBT:
            return FormatWebVTT(ms);
        case VLC_CODEC_HEVC:
            return FormatH265(ms, fmt);
        default:
            return VLC_ENOTSUP;
    }
}

bool hls_codec_IsSupported(const es_format_t *fmt)
{
    return fmt->i_codec == VLC_CODEC_H264 || fmt->i_codec == VLC_CODEC_MP4A ||
           fmt->i_codec == VLC_CODEC_TEXT || fmt->i_codec == VLC_CODEC_WEBVTT || fmt->i_codec == VLC_CODEC_SUBT ||
           fmt->i_codec == VLC_CODEC_HEVC;
}
