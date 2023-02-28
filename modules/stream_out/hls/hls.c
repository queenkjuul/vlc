/*****************************************************************************
 * hls.c: HLS stream output module
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

#include <vlc_block.h>
#include <vlc_list.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>

#include "hls.h"
#include "segments.h"

typedef struct
{
    struct hls_config config;
    struct vlc_list playlists;
} sout_stream_sys_t;

typedef struct
{
    const struct hls_config *config;

    sout_access_out_t *access;
    struct segment_buffer
    {
        block_t *begin;
        block_t **end;
        vlc_tick_t length;
    } ongoing_segment_data;
    hls_segment_queue_t segments;
    struct vlc_list node;
} hls_playlist_t;

static inline void ResetSegmentBuffer(struct segment_buffer *buffer)
{
    buffer->begin = NULL;
    buffer->end = &buffer->begin;
    buffer->length = 0u;
}

static ssize_t AccessOutWrite(sout_access_out_t *access, block_t *block)
{
    hls_playlist_t *playlist = access->p_sys;

    ssize_t wrote = 0;
    while (block != NULL)
    {
        if (playlist->ongoing_segment_data.length + block->i_length >=
            playlist->config->segment_length)
        {
            const int status = hls_segment_queue_NewSegment(
                &playlist->segments,
                playlist->ongoing_segment_data.begin,
                playlist->ongoing_segment_data.length);
            if (unlikely(status != VLC_SUCCESS))
                goto error;

            ResetSegmentBuffer(&playlist->ongoing_segment_data);
        }

        wrote += block->i_buffer;

        block_t *next = block->p_next;

        block->p_next = NULL;
        playlist->ongoing_segment_data.length += block->i_length;
        block_ChainLastAppend(&playlist->ongoing_segment_data.end, block);

        block = next;
    }

    return wrote;
error:
    block_ChainRelease(block);
    return -1;
}

static sout_access_out_t *CreateAccessOut(sout_stream_t *stream,
                                          hls_playlist_t *sys)
{
    sout_access_out_t *access = vlc_object_create(stream, sizeof(*access));
    if (unlikely(access == NULL))
        return NULL;

    access->p_cfg = NULL;
    access->p_module = NULL;
    access->p_sys = sys;
    access->psz_access = strdup("hls");
    access->psz_path = NULL;

    access->pf_control = NULL;
    access->pf_read = NULL;
    access->pf_seek = NULL;
    access->pf_write = AccessOutWrite;
    return access;
}

static hls_playlist_t *CreatePlaylist(sout_stream_t *stream)
{
    hls_playlist_t *playlist = calloc(1, sizeof(*playlist));
    if (unlikely(playlist == NULL))
        return NULL;
    return playlist;
}

static void *Add(sout_stream_t *stream, const es_format_t *fmt)
{
    sout_stream_sys_t *sys = stream->p_sys;

    hls_playlist_t *playlist = CreatePlaylist(stream);

    playlist->access = CreateAccessOut(stream, playlist);
    if (unlikely(playlist->access == NULL))
        goto error;

    playlist->config = &sys->config;

    hls_segment_queue_Init(&playlist->segments, fmt->i_id, &sys->config);

    ResetSegmentBuffer(&playlist->ongoing_segment_data);

    vlc_list_append(&playlist->node, &sys->playlists);

    return playlist;
error:
    free(playlist);
    return NULL;
}

static void Del(sout_stream_t *stream, void *id)
{
    hls_playlist_t *playlist = id;

    sout_AccessOutDelete(playlist->access);

    block_ChainRelease(playlist->ongoing_segment_data.begin);
    hls_segment_queue_Clear(&playlist->segments);

    vlc_list_remove(&playlist->node);
    free(playlist);
}

static int Send(sout_stream_t *stream, void *id, vlc_frame_t *frame)
{
    return VLC_ENOTSUP; // TODO: Implement once the Mux/access output is
                        // introduced.
}

#define SOUT_CFG_PREFIX "sout-hls-"

static int Open(vlc_object_t *this)
{
    sout_stream_t *stream = (sout_stream_t *)this;

    sout_stream_sys_t *sys = malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    static const char *const options[] = {
        "base-url", "numsegs", "seglen", NULL};
    config_ChainParse(stream, SOUT_CFG_PREFIX, options, stream->p_cfg);

    sys->config.base_url = var_GetString(stream, SOUT_CFG_PREFIX "base-url");
    sys->config.max_segments =
        var_GetInteger(stream, SOUT_CFG_PREFIX "numsegs");
    sys->config.segment_length =
        VLC_TICK_FROM_SEC(var_GetInteger(stream, SOUT_CFG_PREFIX "seglen"));

    vlc_list_init(&sys->playlists);

    static const struct sout_stream_operations ops = {
        .add = Add,
        .del = Del,
        .send = Send,
    };
    stream->ops = &ops;
    stream->p_sys = sys;

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *this)
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;

    hls_config_Clean(&sys->config);

    free(sys);
}

#define BASEURL_TEXT N_("Base URL/Path")
#define NUMSEGS_LONGTEXT N_("Number of segments to include in index")
#define NUMSEGS_TEXT N_("Number of segments")
#define SEGLEN_LONGTEXT N_("Length of segments in seconds")
#define SEGLEN_TEXT N_("Segment length (sec)")

// clang-format off
vlc_module_begin()
    set_shortname("HLS")
    set_description(N_("HLS stream output"))
    set_capability("sout output", 50)
    add_shortcut("hls")
    set_subcategory(SUBCAT_SOUT_STREAM)

    add_string(SOUT_CFG_PREFIX "base-url", "", BASEURL_TEXT, BASEURL_TEXT)
    add_integer(SOUT_CFG_PREFIX "numsegs", 5, NUMSEGS_TEXT, NUMSEGS_LONGTEXT)
    add_integer(SOUT_CFG_PREFIX "seglen", 4, SEGLEN_TEXT, SEGLEN_LONGTEXT)

    set_callbacks(Open, Close)
vlc_module_end()
