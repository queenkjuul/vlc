#include "common.h"

#include <vlc_fs.h>
#include <vlc_block.h>

#include <assert.h>
#include <fcntl.h>

static hls_io *hls_io_NewFile( const char *path, int flags );
static hls_io *hls_io_NewBlockChain();

hls_io *hls_io_New( const char *path, int flags )
{
    if ( path == NULL )
        return hls_io_NewBlockChain();
    return hls_io_NewFile( path, flags );
}

/*****************************************************************************
 * IO: File
 *****************************************************************************/
typedef struct
{
    char *path;
    int flags;
    int fd;
} hls_io_FileData;

static int file_open( hls_io *self )
{
    hls_io_FileData *sys = self->sys;
    assert( sys->path != NULL );
    assert( sys->fd == -1 );

    sys->fd = vlc_open( sys->path, sys->flags, 0666 );
    return sys->fd == -1 ? VLC_EGENERIC : VLC_SUCCESS;
}

static void file_close( hls_io *self )
{
    hls_io_FileData *sys = self->sys;
    assert( sys->fd != -1 );

    vlc_close( sys->fd );
    sys->fd = -1;
}

static block_t *file_consume_block( hls_io *self, block_t *block )
{
    const hls_io_FileData *sys = self->sys;
    assert( sys->fd != -1 );

    const ssize_t wrote = write( sys->fd, block->p_buffer, block->i_buffer );

    if ( likely( wrote >= 0 ) )
        self->size += wrote;

    const ssize_t bytes_left = block->i_buffer - wrote;
    block_t *ret = NULL;
    if ( bytes_left > 0 )
    {
        ret = block_Alloc( bytes_left );
        memcpy( ret->p_buffer, block->p_buffer + wrote, bytes_left );
    }

    block_Release( block );

    return ret;
}

struct hls_io_FileRContext
{
    int fd;
};

static void *file_new_reading_context( const hls_io *self )
{
    const hls_io_FileData *sys = self->sys;

    struct hls_io_FileRContext *ret = calloc( 1, sizeof( *ret ) );
    if ( unlikely( ret == NULL ) )
        return NULL;

    ret->fd = open( sys->path, O_RDONLY | O_LARGEFILE );
    if ( unlikely( ret->fd == -1 ) )
    {
        free( ret );
        return NULL;
    }

    return ret;
}

static void file_release_reading_context( void *opaque )
{
    struct hls_io_FileRContext *context = opaque;

    assert( context->fd > 0 );
    close( context->fd );

    free( context );
}

static ssize_t file_read( void *reading_context, uint8_t buffer[], size_t size )
{
    const struct hls_io_FileRContext *context = reading_context;
    assert( context->fd != -1 );
    return read( context->fd, buffer, size );
}

static int file_unlink( hls_io *self )
{
    const hls_io_FileData *sys = self->sys;
    assert( sys->path != NULL );

    return vlc_unlink( sys->path );
}

static void file_release( hls_io *self )
{
    hls_io_FileData *sys = self->sys;
    if ( sys->fd != -1 )
        vlc_close( sys->fd );
    free( sys->path );
    free( sys );
}

static hls_io *hls_io_NewFile( const char *path, int flags )
{
    hls_io *ret = calloc( 1, sizeof( *ret ) );
    if ( unlikely( ret == NULL ) )
        return NULL;

    hls_io_FileData *sys = malloc( sizeof( *sys ) );
    if ( unlikely( sys == NULL ) )
    {
        goto err;
    }

    *sys =
        ( hls_io_FileData ){ .path = strdup( path ), .flags = flags, .fd = -1 };
    if ( unlikely( sys->path == NULL ) )
    {
        goto err;
    }
    ret->sys = sys;

    ret->ops =
        ( hls_io_ops ){ .open = file_open,
                        .consume_block = file_consume_block,
                        .new_reading_context = file_new_reading_context,
                        .release_reading_context = file_release_reading_context,
                        .read = file_read,
                        .close = file_close,
                        .unlink = file_unlink,
                        .release = file_release };
    return ret;

err:
    free( ret );
    return NULL;
}

/*****************************************************************************
 * IO: BlockChain
 *****************************************************************************/

typedef struct
{
    block_t *begin;
    block_t *end;
} hls_io_BlockChainData;

static int blockchain_open( hls_io *self )
{
    printf( "OPEN\n" );
    const hls_io_BlockChainData *sys = self->sys;
    assert( sys->begin == NULL );
    assert( sys->end == NULL );
    return VLC_SUCCESS;
}

static void blockchain_close( hls_io *self ) { (void)self; }

static block_t *blockchain_consume_block( hls_io *self, block_t *block )
{
    assert( block != NULL );
    hls_io_BlockChainData *sys = self->sys;

    block_ChainAppend( &sys->end, block );

    sys->end = block;

    if ( sys->begin == NULL )
        sys->begin = block;

    self->size += block->i_buffer;

    return NULL;
}

struct hls_io_BlockChainRContext
{
    const block_t *current_block;

    /* If read stop in the middle of a block buffer, this cursor keeps track of
     * the byte where copy stopped. */
    size_t block_cursor;
};

static void *blockchain_new_reading_context( const hls_io *self )
{
    printf( "RCONTEXT\n" );
    const hls_io_BlockChainData *sys = self->sys;

    struct hls_io_BlockChainRContext *ret = malloc( sizeof( *ret ) );
    if ( unlikely( ret == NULL ) )
        return NULL;

    *ret = ( struct hls_io_BlockChainRContext ){ .current_block = sys->begin,
                                                 .block_cursor = 0 };

    return ret;
}

static ssize_t
blockchain_read( void *reading_context, uint8_t buffer[], size_t size )
{
    printf( "READ\n" );
    struct hls_io_BlockChainRContext *context = reading_context;

    size_t total = 0;
    while ( context->current_block != NULL && size != 0 )
    {
        const block_t *current = context->current_block;

        const size_t copy =
            __MIN( size, current->i_buffer - context->block_cursor );

        memcpy( buffer, current->p_buffer + context->block_cursor, copy );

        if ( copy == size )
        {
            /* We need to stop reading in the middle of a block. We then
             * remember the block buffer copy position */
            context->block_cursor = copy;
        }
        else
        {
            context->block_cursor = 0;
            context->current_block = current->p_next;
        }

        size -= copy;
        buffer += copy;
        total += copy;
    }
    return total;
}

static int blockchain_unlink( hls_io *self )
{
    return VLC_SUCCESS;
    (void)self;
}

static void blockchain_release( hls_io *self )
{
    printf( "RELEASE\n" );
    hls_io_BlockChainData *sys = self->sys;
    if ( sys->begin != NULL )
        block_ChainRelease( sys->begin );

    free( self->sys );
}

static hls_io *hls_io_NewBlockChain()
{
    printf( "NEW\n" );
    hls_io *ret = calloc( 1, sizeof( *ret ) );
    if ( unlikely( ret == NULL ) )
        return NULL;

    ret->sys = calloc( 1, sizeof( hls_io_BlockChainData ) );
    if ( unlikely( ret->sys == NULL ) )
    {
        free( ret );
        return NULL;
    }

    ret->ops =
        ( hls_io_ops ){ .open = blockchain_open,
                        .consume_block = blockchain_consume_block,
                        .new_reading_context = blockchain_new_reading_context,
                        .release_reading_context = free,
                        .read = blockchain_read,
                        .close = blockchain_close,
                        .unlink = blockchain_unlink,
                        .release = blockchain_release };
    return ret;
}

