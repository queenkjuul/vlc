/*****************************************************************************
 * hls.c: VLC HLS plugin module
 *****************************************************************************
 * Copyright (C) 2003-2021 VLC authors and VideoLAN
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
#include "common.h"

#include <vlc_plugin.h>
#include <vlc_sout.h>


static const char *const sout_options[] = { NULL };

typedef struct
{
    sout_mux_t *mux;
    hls_sout_callbacks callbacks;
} sout_stream_sys_t;

/*****************************************************************************
 * Module callbacks
 *****************************************************************************/

static void *Add( sout_stream_t *p_stream, const es_format_t *p_fmt )
{
    sout_stream_sys_t *sys = p_stream->p_sys;
    return sout_MuxAddStream( sys->mux, p_fmt );
}

static void Del( sout_stream_t *p_stream, void *id )
{
    sout_stream_sys_t *sys = p_stream->p_sys;
    sout_MuxDeleteStream( sys->mux, (sout_input_t *)id );
}

static int Send( sout_stream_t *p_stream, void *id, block_t *p_buffer )
{
    sout_stream_sys_t *sys = p_stream->p_sys;
    return sout_MuxSendBuffer( sys->mux, (sout_input_t *)id, p_buffer );
}

static int Control(sout_stream_t *stream, int query, va_list args)
{
    (void)stream;
    //const sout_stream_sys_t *sys = stream->p_sys;

    switch (query)
    {
        case SOUT_STREAM_IS_SYNCHRONOUS:
            *va_arg(args, bool *) = false; // TODO
            break;

        default:
            return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static void Flush( sout_stream_t *p_stream, void *id )
{
    sout_stream_sys_t *sys = p_stream->p_sys;
    sout_MuxFlush( sys->mux, (sout_input_t *)id );
}

static const struct sout_stream_operations ops = {
    Add, Del, Send, Control, Flush,
};

static int SoutOpen( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;

    sout_stream_sys_t *sys = stream->p_sys = calloc( 1, sizeof( *sys ) );
    if ( unlikely( sys == NULL ) )
        return VLC_ENOMEM;

    config_ChainParse( stream, SOUT_CFG_PREFIX, sout_options, stream->p_cfg );

    sout_access_out_t *access = sout_AccessOutNew( stream, "livehttp", NULL );
    if ( unlikely( access == NULL ) )
    {
        msg_Err( stream, "Can't create the livehttp access-out module." );
        goto err;
    }

    sys->mux = sout_MuxNew( access, "ts" );
    if ( unlikely( sys->mux == NULL ) )
    {
        msg_Err( stream, "Can't create ts muxer." );
        goto err;
    }

    var_Create(access, "sout-callbacks", );

    stream->ops = &ops;
    return VLC_SUCCESS;
err:
    if ( access != NULL )
        sout_AccessOutDelete( access );
    free( sys );
    return VLC_EGENERIC;
}

static void SoutClose( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;
    sout_access_out_t *access = sys->mux->p_access;

    sout_MuxDelete( sys->mux );
    sout_AccessOutDelete( access );

    free( sys );
}
