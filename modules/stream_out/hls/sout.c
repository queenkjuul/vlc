/*****************************************************************************
 * sout.c: HLS stream output module
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
#include <vlc_url.h>

#include <assert.h>

static const char *const sout_options[] = { NULL };

typedef struct
{
    sout_mux_t *mux;

    httpd_host_t *httpd_host;

    httpd_url_t *httpd_aco_index_url;
    hls_index index;
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

    vlc_url_t url;
    if (vlc_UrlParse(&url, segment->psz_uri) != VLC_SUCCESS)
        return VLC_EGENERIC;

    segment_http_context *context = calloc( 1, sizeof( *context ) );
    if ( unlikely( context == NULL ) )
        return VLC_ENOMEM;

    context->url = httpd_UrlNew( sys->httpd_host, url.psz_path, NULL, NULL );
    if ( unlikely( context->url == NULL ) )
    {
        free( context );
        vlc_UrlClean( &url );
        return VLC_ENOMEM;
    }
    httpd_UrlCatch( context->url, HTTPD_MSG_GET,
                    (httpd_callback_t)url_segment_cb, (void *)segment );

    vlc_queue_Enqueue( &sys->segments_http_context, context );

    vlc_UrlClean( &url );
    return VLC_SUCCESS;
}

static void IndexUpdated( void *opaque,
                          const sout_access_out_t *access,
                          const hls_io *index_io )
{
    sout_stream_sys_t *sys = opaque;

    assert( sys->mux->p_access == access );

    vlc_mutex_lock( &sys->index.lock );
    sys->index.handle = index_io;
    vlc_mutex_unlock( &sys->index.lock );
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

int SoutOpen( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;

    sout_stream_sys_t *sys = stream->p_sys = calloc( 1, sizeof( *sys ) );
    if ( unlikely( sys == NULL ) )
        return VLC_ENOMEM;

    config_ChainParse( stream, SOUT_CFG_PREFIX, sout_options, stream->p_cfg );

    vlc_queue_Init( &sys->segments_http_context,
                    offsetof( segment_http_context, next ) );

    vlc_mutex_init( &sys->index.lock );

    sys->httpd_host = vlc_http_HostNew( this );
    if ( unlikely( sys->httpd_host == NULL ) )
    {
        free( sys );
        return VLC_ENOMEM;
    }

    sys->httpd_aco_index_url =
        httpd_UrlNew( sys->httpd_host, "/index.m3u8", NULL, NULL );
    if ( unlikely( sys->httpd_aco_index_url == NULL ) )
        goto err;

    httpd_UrlCatch( sys->httpd_aco_index_url, HTTPD_MSG_GET,
                    (httpd_callback_t)url_index_cb, (void *)&sys->index );

    sys->callbacks =
        ( struct hls_sout_callbacks ){ .sout_sys = sys,
                                       .segment_added = SegmentAdded,
                                       .segment_removed = SegmentRemoved,
                                       .index_updated = IndexUpdated };

    var_Create( this, HLS_SOUT_CALLBACKS_VAR, VLC_VAR_ADDRESS );
    var_SetAddress( this, HLS_SOUT_CALLBACKS_VAR, &sys->callbacks );

    sout_access_out_t *access = sout_AccessOutNew(
        stream,
        "livehttp{seglen=4,pace=true,delsegs=true,numsegs=5,index-path=/tmp/"
        "mystream.m3u8,segment-url=http://localhost:8080/########.ts}",
        "/tmp/########.ts" );

    if ( unlikely( access == NULL ) )
    {
        msg_Err( stream, "Can't create the livehttp access-out module." );
        goto err;
    }

    sys->mux = sout_MuxNew( access, "ts" );
    if ( unlikely( sys->mux == NULL ) )
    {
        msg_Err( stream, "Can't create ts muxer." );
        sout_AccessOutDelete( access );
        goto err;
    }

    stream->ops = &ops;
    return VLC_SUCCESS;
err:
    if (sys->httpd_aco_index_url)
        httpd_UrlDelete(sys->httpd_aco_index_url);
    httpd_HostDelete( sys->httpd_host );
    free( sys );
    return VLC_EGENERIC;
}

void SoutClose( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;
    sout_access_out_t *access = sys->mux->p_access;

    sout_MuxDelete( sys->mux );

    var_Destroy( access, HLS_SOUT_CALLBACKS_VAR );
    sout_AccessOutDelete( access );
    // All segments should have been cleaned up after the access release.
    assert( vlc_queue_IsEmpty( &sys->segments_http_context ) );

    httpd_UrlDelete(sys->httpd_aco_index_url);
    httpd_HostDelete( sys->httpd_host );

    free( sys );
}
