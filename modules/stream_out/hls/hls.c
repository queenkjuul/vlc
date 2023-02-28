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

typedef struct
{
    struct vlc_list playlists;
} sout_stream_sys_t;

typedef struct
{
    struct vlc_list node;
} hls_playlist_t;

static void *Add(sout_stream_t *stream, const es_format_t *fmt)
{
    sout_stream_sys_t *sys = stream->p_sys;

    hls_playlist_t *playlist = calloc(1, sizeof(*playlist));
    if (unlikely(playlist == NULL))
        return NULL;

    vlc_list_append(&playlist->node, &sys->playlists);

    return playlist;
}

static void Del(sout_stream_t *stream, void *id)
{
    hls_playlist_t *playlist = id;

    vlc_list_remove(&playlist->node);
    free(playlist);
}

static int Send(sout_stream_t *stream, void *id, vlc_frame_t *frame)
{
    return VLC_ENOTSUP; // TODO: Implement once the Mux/access output is introduced.
}

static int Open(vlc_object_t *this)
{
    sout_stream_t *stream = (sout_stream_t *)this;

    sout_stream_sys_t *sys = malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

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

    free(sys);
}

// clang-format off
vlc_module_begin()
    set_shortname("HLS")
    set_description(N_("HLS stream output"))
    set_capability("sout output", 50)
    add_shortcut("hls")
    set_subcategory(SUBCAT_SOUT_STREAM)

    set_callbacks(Open, Close)
vlc_module_end()
