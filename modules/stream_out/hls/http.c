#include "common.h"

/*****************************************************************************
 * HTTP: Utils
 *****************************************************************************/

static uint8_t *io_read_all( const hls_io *io )
{

    uint8_t *ret = malloc( io->size );
    if ( unlikely( ret == NULL ) )
        return NULL;

    void *reading_context = io->ops.new_reading_context( io );
    if ( reading_context == NULL )
        goto err;

    size_t to_read = io->size;
    size_t cursor = 0;
    while ( to_read != 0 )
    {
        const ssize_t read =
            io->ops.read( reading_context, ret + cursor, to_read );

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

static void set_http_headers( httpd_message_t *answer )
{
    httpd_MsgAdd( answer, "Content-type", "application/vnd.apple.mpegurl" );
    httpd_MsgAdd( answer, "Cache-Control", "no-cache" );
    httpd_MsgAdd( answer, "Connection", "close" );
    httpd_MsgAdd( answer, "Access-Control-Allow-Origin", "*" );
    httpd_MsgAdd( answer, "Access-Control-Expose-Headers", "Content-Length" );
}

int url_segment_cb( const hls_segment *segment,
                    httpd_client_t *cl,
                    httpd_message_t *answer,
                    const httpd_message_t *query )
{
    if ( !answer || !query || !cl )
        return VLC_SUCCESS;

    const hls_io *io = segment->io_handle;

    printf( "fetching %s\n", segment->psz_filename );

    answer->i_body = io->size;
    answer->p_body = io_read_all( io );
    if ( answer->p_body == NULL )
        return VLC_EGENERIC;

    answer->i_proto = HTTPD_PROTO_HTTP;
    answer->i_version = 0;
    answer->i_type = HTTPD_MSG_ANSWER;
    answer->i_status = 200;

    set_http_headers( answer );
    return VLC_SUCCESS;
}
