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

#include "common.h"

#include <vlc_plugin.h>
#include <vlc_queue.h>
#include <vlc_sout.h>

#include <assert.h>

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
    sout_mux_t *mux;

    httpd_host_t *httpd_host;
    vlc_queue_t segments_http_context;
    struct hls_sout_callbacks callbacks;
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

static int Control( sout_stream_t *stream, int query, va_list args )
{
    (void)stream;
    // const sout_stream_sys_t *sys = stream->p_sys;

    switch ( query )
    {
    case SOUT_STREAM_IS_SYNCHRONOUS:
        *va_arg( args, bool * ) = false; // TODO
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

typedef struct segment_http_context
{
    httpd_url_t *url;
    struct segment_http_context *next;
} segment_http_context;

static int SegmentAdded( void *opaque,
                         const sout_access_out_t *access,
                         const hls_segment *segment )
{
    sout_stream_sys_t *sys = opaque;

    assert( sys->mux->p_access == access );

    segment_http_context *context = calloc( 1, sizeof( *context ) );
    if ( unlikely( context == NULL ) )
        return VLC_ENOMEM;

    char *segment_url;
    const int result =
        asprintf( &segment_url, "/%" PRIu32 ".ts", segment->i_segment_number );
    if ( unlikely( result == -1 ) )
    {
        free( context );
        return VLC_ENOMEM;
    }

    context->url = httpd_UrlNew( sys->httpd_host, segment_url, NULL, NULL );
    if ( unlikely( context->url == NULL ) )
    {
        free( context );
        free( segment_url );
        return VLC_ENOMEM;
    }
    httpd_UrlCatch( context->url, HTTPD_MSG_GET,
                    (httpd_callback_t)url_segment_cb, (void *)segment );

    vlc_queue_Enqueue( &sys->segments_http_context, context );

    free( segment_url );
    return VLC_SUCCESS;
}

static void SegmentRemoved( void *opaque,
                            const sout_access_out_t *access,
                            const hls_segment *segment )
{
    sout_stream_sys_t *sys = opaque;

    assert( sys->mux->p_access == access );

    segment_http_context *context =
        vlc_queue_Dequeue( &sys->segments_http_context );

    // Should not be called when the queue is empty.
    assert( context );

    httpd_UrlDelete( context->url );
    free( context );

    (void)segment;
}

static int Open( vlc_object_t *this )
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

    sys->httpd_host = vlc_http_HostNew( VLC_OBJECT( access ) );
    if ( unlikely( sys->httpd_host == NULL ) )
    {
        sout_MuxDelete( sys->mux );
        goto err;
    }

    vlc_queue_Init( &sys->segments_http_context,
                    offsetof( segment_http_context, next ) );

    sys->callbacks =
        ( struct hls_sout_callbacks ){ .sout_sys = sys,
                                       .segment_added = SegmentAdded,
                                       .segment_removed = SegmentRemoved };

    var_Create( access, HLS_SOUT_CALLBACKS_VAR, VLC_VAR_ADDRESS );
    var_SetAddress( access, HLS_SOUT_CALLBACKS_VAR, &sys->callbacks );

    stream->ops = &ops;
    return VLC_SUCCESS;
err:
    if ( access != NULL )
        sout_AccessOutDelete( access );
    free( sys );
    return VLC_EGENERIC;
}

static void Close( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;
    sout_access_out_t *access = sys->mux->p_access;

    sout_MuxDelete( sys->mux );

    var_Destroy( access, HLS_SOUT_CALLBACKS_VAR );
    sout_AccessOutDelete( access );
    // All segments should have been cleaned up after the access release.
    assert( vlc_queue_IsEmpty( &sys->segments_http_context ) );

    httpd_HostDelete( sys->httpd_host );

    free( sys );
}
