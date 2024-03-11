/*****************************************************************************
 * emjsfetch.c: emscripten js fetch api plugin
 *****************************************************************************
 * Copyright (C) 2022 VLC authors Videolabs, and VideoLAN
 *
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
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_access.h>
#include <vlc_threads.h>
#include <stdalign.h>

#include <emscripten.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

typedef struct
{   
    char *url;
} access_sys_t;

static block_t *BlockFetch(stream_t *p_access, bool *restrict eof)
{
    access_sys_t *p_sys = p_access->p_sys;
    int rc, fz;
    void *ebuf;
    block_t *pkt = NULL;

    msg_Dbg( p_access, "fetch: start(%s)", p_sys->url);
    emscripten_wget_data(p_sys->url, &ebuf, &fz, &rc);
    if (rc != 0) {
       msg_Dbg( p_access, "fetch: unable to fetch(%s), err: %d", p_sys->url, rc);
       return pkt;
    }

    msg_Dbg( p_access, "fetch: %d byte(s)", fz);

    pkt = block_Alloc( fz );
    if ( unlikely( pkt == NULL ) ) {   
       msg_Dbg( p_access, "fetch: block_Alloc(%s) failed", p_sys->url);
       return NULL;
    }
    memcpy(pkt->p_buffer, ebuf, fz);
    pkt->i_buffer = fz;

    free(ebuf);
    *eof = true;
    return pkt;
}

static int Control( stream_t *p_access, int i_query, va_list args )
{
    int i_ret = VLC_SUCCESS;

    switch( i_query )
    {
        case STREAM_CAN_SEEK:
        case STREAM_CAN_FASTSEEK:
        case STREAM_CAN_PAUSE:
        case STREAM_CAN_CONTROL_PACE:
            *va_arg( args, bool * ) = false;
            break;
        case STREAM_GET_PTS_DELAY:
            *va_arg( args, vlc_tick_t * ) = VLC_TICK_FROM_MS(
                   var_InheritInteger(p_access, "network-caching") );
            break;
        default:
            i_ret = VLC_EGENERIC;
            break;
    }

    return i_ret;
}

static void EmFetchClose (vlc_object_t * p_this) {
    stream_t *p_access = (stream_t*)p_this;
    access_sys_t *p_sys = p_access->p_sys;
    free(p_sys->url);
}

static int EmFetchOpen( vlc_object_t *p_this ) {
    stream_t *p_access = (stream_t*)p_this;

    access_sys_t *p_sys = vlc_obj_malloc(p_this, sizeof (*p_sys));
    if (unlikely(p_sys == NULL))
        return VLC_ENOMEM;

    msg_Dbg( p_access, "fetch: base url='%s'", p_access->psz_url);

    if (p_access->psz_url[0] == 'f') {
       p_sys->url = strdup(p_access->psz_url + 8);
    } else {
       p_sys->url = strdup(p_access->psz_url);
    }
    p_access->p_sys = p_sys;
    
    msg_Dbg( p_access, "fetch: url='%s'", p_sys->url);

    p_access->pf_block = BlockFetch;
    p_access->pf_control = Control;

    return VLC_SUCCESS;
}

vlc_module_begin ()
    set_shortname( N_("Fetch API") )
    set_description( N_("Emscripten module to allow fetching remote resources") )
    set_capability( "access", 0 )
    //set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_ACCESS )
    add_shortcut( "fetch", "https", "http" )
    set_callbacks( EmFetchOpen, EmFetchClose )
vlc_module_end()
