/*****************************************************************************
 * segments.c
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

#include <assert.h>

#include <vlc_common.h>

#include <vlc_list.h>

#include "hls.h"
#include "segments.h"
#include "storage.h"

static void hls_segment_Destroy(hls_segment_t *segment)
{
    hls_storage_Destroy(segment->storage);
    free(segment->url);
    free(segment);
}

void hls_segment_queue_Init(hls_segment_queue_t *queue,
                            unsigned int id,
                            const struct hls_config *config)
{
    queue->id = id;
    queue->total_segments = 0;
    queue->max_segments = config->max_segments;
    queue->base_url = config->base_url;
    vlc_list_init(&queue->segments);
}

void hls_segment_queue_Clear(hls_segment_queue_t *queue)
{
    hls_segment_t *it;
    hls_segment_queue_Foreach(queue, it) { hls_segment_Destroy(it); }
}

int hls_segment_queue_NewSegment(hls_segment_queue_t *queue,
                                 block_t *content,
                                 vlc_tick_t length)
{
    hls_segment_t *segment = malloc(sizeof(*segment));
    if (unlikely(segment == NULL))
        return VLC_ENOMEM;

    segment->storage = hls_storage_FromBlocks(content, HLS_STORAGE_TYPE_MEMORY);
    if (unlikely(segment->storage == NULL))
        goto nomem;

    segment->id = queue->total_segments;
    segment->length = length;

    if (asprintf(&segment->url,
                 "%s/playlist-%u-%u.ts",
                 queue->base_url,
                 queue->id,
                 segment->id) == -1)
        goto nomem;

    if (queue->max_segments <= queue->total_segments)
    {
        hls_segment_t *old = hls_segment_GetFirst(queue);
        assert(old != NULL);
        vlc_list_remove(&old->priv_node);
        hls_segment_Destroy(old);
    }

    ++queue->total_segments;
    vlc_list_append(&segment->priv_node, &queue->segments);
    return VLC_SUCCESS;
nomem:
    if (segment->storage != NULL)
        hls_storage_Destroy(segment->storage);
    free(segment);
    return VLC_ENOMEM;
}
