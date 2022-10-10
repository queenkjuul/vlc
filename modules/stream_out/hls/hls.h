/*****************************************************************************
 * hls.h
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
#ifndef HLS_H
#define HLS_H

struct hls_config
{
    char *base_url;
    char *outdir;
    unsigned int max_segments;
    bool pace;
    vlc_tick_t segment_length;
    size_t max_memory;
};

#define BYTES_FROM_KB(x) ((x) * 1000)
#define BYTES_TO_KB(x) ((x) / 1000)

static inline void hls_config_Clean(struct hls_config *config)
{
    free(config->base_url);
    free(config->outdir);
}

static inline bool
hls_config_IsMemStorageEnabled(const struct hls_config *config)
{
    return config->outdir == NULL;
}

typedef struct hls_pacer {
    bool is_enabled;
    vlc_tick_t deadline;
} hls_pacer_t;

static inline void hls_pacer_Pace(hls_pacer_t *pacer,
                                        vlc_tick_t pace_for)
{
    if (!pacer->is_enabled)
        return;

    const vlc_tick_t now = vlc_tick_now();

    vlc_tick_t last_deadline = pacer->deadline;
    if (last_deadline == VLC_TICK_INVALID)
        last_deadline = now;

    pacer->deadline = now + (pace_for - (now - last_deadline));
    printf("Pacer for %ld, deadline =  %ld",
           MS_FROM_VLC_TICK(pace_for),
           MS_FROM_VLC_TICK(pacer->deadline));
}

#endif
