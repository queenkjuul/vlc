/*****************************************************************************
 * debug.c: dummy stream output module
 *****************************************************************************
 * Copyright (C) 2003-2004 VLC authors and VideoLAN
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
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

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>

#include <vlc_block.h>
#include <vlc_messages.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>

typedef struct 
{
    void *next_id;
    char *es_id;
}sout_stream_id_sys_t;

static void *
Add(sout_stream_t *p_stream, const es_format_t *p_fmt, const char *es_id)
{
    msg_Info(p_stream, "Added %s.", es_id);
    sout_stream_id_sys_t *id = malloc(sizeof(*id));
    id->es_id = strdup(es_id);
    id->next_id = sout_StreamIdAdd(p_stream->p_next, p_fmt, es_id);
    return id;
}

static void Del(sout_stream_t *p_stream, void *id)
{
    sout_stream_id_sys_t *sys = id;
    sout_StreamIdDel(p_stream->p_next, sys->next_id);
    msg_Info(p_stream, "Deleted %s.", sys->es_id);
    free(sys->es_id);
    free(sys);
}

static int Send(sout_stream_t *p_stream, void *id, block_t *p_buffer)
{
    sout_stream_id_sys_t *sys = id;
    return sout_StreamIdSend(p_stream->p_next, sys->next_id, p_buffer);
}

static int Control( sout_stream_t *p_stream, int i_query, va_list args )
{
    return sout_StreamControl(p_stream->p_next, i_query, args);
}

static const struct sout_stream_operations ops = {
    Add,
    Del,
    Send,
    Control,
    NULL,
    NULL,
};

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open(vlc_object_t *p_this)
{
    sout_stream_t *p_stream = (sout_stream_t *)p_this;

    p_stream->ops = &ops;
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
vlc_module_begin() set_description(N_("Debug stream output"))
    set_capability("sout filter", 50) add_shortcut("debug")
        set_callback(Open) vlc_module_end()
