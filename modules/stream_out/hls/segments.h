/*****************************************************************************
 * segments.h
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
#ifndef HLS_SEGMENTS_H
#define HLS_SEGMENTS_H

struct hls_storage;
struct hls_config;

typedef struct hls_segment
{
    char *url;
    unsigned int id;
    vlc_tick_t length;

    struct hls_storage *storage;

    httpd_url_t *http_url;

    struct vlc_list priv_node;
} hls_segment_t;


struct hls_segment_queue_config {
    unsigned int id;
    unsigned int max_segments;
    const char *base_url;

    httpd_host_t *httpd_ref;
    httpd_callback_t httpd_callback;
};

typedef struct
{
    struct hls_segment_queue_config config;
    unsigned int total_segments;

    struct vlc_list segments;
} hls_segment_queue_t;

#define hls_segment_queue_Foreach(queue, it)                                   \
    vlc_list_foreach(it, &(queue)->segments, priv_node)
#define hls_segment_GetFirst(queue)                                            \
    vlc_list_first_entry_or_null(&(queue)->segments, hls_segment_t, priv_node);

void hls_segment_queue_Init(hls_segment_queue_t *queue,
                            const struct hls_segment_queue_config *);
void hls_segment_queue_Clear(hls_segment_queue_t *);
int hls_segment_queue_NewSegment(hls_segment_queue_t *,
                                 block_t *content,
                                 vlc_tick_t length);

#endif
