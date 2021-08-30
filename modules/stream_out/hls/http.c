/*****************************************************************************
 * http.c: HLS httpd callbacks
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

#include "common.h"

static uint8_t *hls_io_ReadAll( const hls_io *io )
{

    uint8_t *ret = malloc( io->size );
    if ( unlikely( ret == NULL ) )
    {
        return NULL;
    }

    void *reading_context = io->ops.new_reading_context( io );
    if ( reading_context == NULL )
    {
        goto err;
    }

    size_t to_read = io->size;
    size_t cursor = 0;
    while ( to_read != 0 )
    {
        const ssize_t read = io->ops.read( reading_context, ret + cursor, to_read );

        if ( read == -1 )
        {
            io->ops.release_reading_context( reading_context );
            goto err;
        }

        cursor += read;
        to_read -= read;
    }

    io->ops.release_reading_context( reading_context );
    return ret;
err:
    free( ret );
    return NULL;
}

static inline void hls_http_SetHeaders( httpd_message_t *answer )
{
    httpd_MsgAdd( answer, "Content-Length", "%d", answer->i_body );
    httpd_MsgAdd( answer, "Content-Type", "application/vnd.apple.mpegurl" );
    httpd_MsgAdd( answer, "Cache-Control", "no-cache" );
    httpd_MsgAdd( answer, "Connection", "close" );
    httpd_MsgAdd( answer, "Access-Control-Allow-Origin", "*" );
    httpd_MsgAdd( answer, "Access-Control-Expose-Headers", "Content-Length" );
}

int hls_http_SegmentCallback( httpd_callback_sys_t *sys,
                              httpd_client_t *cl,
                              httpd_message_t *answer,
                              const httpd_message_t *query )
{
    if ( !answer || !query || !cl )
    {
        return VLC_SUCCESS;
    }

    const hls_segment *segment = (hls_segment *)sys;

    const hls_io *io = segment->io_handle;

    printf( "fetching %s\n", segment->psz_filename );

    answer->i_body = io->size;
    answer->p_body = hls_io_ReadAll( io );
    if ( answer->p_body == NULL )
    {
        return VLC_EGENERIC;
    }

    answer->i_proto = HTTPD_PROTO_HTTP;
    answer->i_version = 0;
    answer->i_type = HTTPD_MSG_ANSWER;
    answer->i_status = 200;

    hls_http_SetHeaders( answer );
    return VLC_SUCCESS;
}

int hls_http_IndexCallback( httpd_callback_sys_t *sys,
                            httpd_client_t *cl,
                            httpd_message_t *answer,
                            const httpd_message_t *query )
{
    if ( !answer || !query || !cl )
    {
        return VLC_SUCCESS;
    }

    hls_index *index = (hls_index *)sys;

    vlc_mutex_lock( &index->lock );

    const hls_io *io = index->handle;
    if ( io == NULL )
    {
        goto err;
    }

    answer->i_body = io->size;
    answer->p_body = hls_io_ReadAll( io );
    if ( answer->p_body == NULL )
    {
        goto err;
    }

    vlc_mutex_unlock( &index->lock );

    answer->i_proto = HTTPD_PROTO_HTTP;
    answer->i_version = 0;
    answer->i_type = HTTPD_MSG_ANSWER;
    answer->i_status = 200;

    hls_http_SetHeaders( answer );

    return VLC_SUCCESS;

err:
    vlc_mutex_unlock( &index->lock );
    return VLC_EGENERIC;
}
