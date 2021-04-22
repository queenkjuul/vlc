/*****************************************************************************
 * hls.c: HLS stream output module
 *****************************************************************************
 * Copyright (C) 2003-2011 VLC authors and VideoLAN
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

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>

#include <vlc_plugin.h>
#include <vlc_sout.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int Open( vlc_object_t * );
static void Close( vlc_object_t * );

#define SOUT_CFG_PREFIX "sout-hls-"

/* clang-format off */
vlc_module_begin()
    set_shortname( "HLS" )
    set_description( N_( "HLS stream output" ) )
    set_capability( "sout output", 50 )
    add_shortcut( "hls" )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_STREAM )

    // add_string( SOUT_CFG_PREFIX "access", "", ACCESS_TEXT, ACCESS_LONGTEXT, false );
    set_callbacks( Open, Close )
vlc_module_end();
/* clang-format on */

static const char *const sout_options[] = { NULL };

typedef struct
{
} sout_stream_sys_t;

/*****************************************************************************
 * Module callbacks
 *****************************************************************************/

static const struct sout_stream_operations ops = {
    NULL, /* Add */
    NULL, /* Del */
    NULL, /* Send */
    NULL, /* Control */
    NULL, /* Flush */
};

static int Open( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;

    sout_stream_sys_t *sys = stream->p_sys = calloc( 1, sizeof( *sys ) );
    if ( unlikely( sys == NULL ) )
        return VLC_ENOMEM;

    config_ChainParse( stream, SOUT_CFG_PREFIX, sout_options, stream->p_cfg );

    stream->ops = &ops;
    return VLC_SUCCESS;
}

static void Close( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;
    free( sys );
}
