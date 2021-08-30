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

#include "common.h"

#include <vlc_block.h>
#include <vlc_list.h>
#include <vlc_memstream.h>
#include <vlc_plugin.h>
#include <vlc_queue.h>
#include <vlc_sout.h>
#include <vlc_url.h>

#include <assert.h>
#include <fcntl.h>

#include <vlc_tracer.h>

struct hls_media
{
    es_format_t fmt;
    sout_mux_t *mux;
    sout_input_t *mux_input;

    bool is_main_media;

    httpd_url_t *httpd_index_url; //<
    hls_index index_handler;      //<

    httpd_host_t *httpd_host;  //< Reference on the http host held in sys.
    vlc_queue_t segment_queue; //< Holds exposed segments.

    struct hls_sout_callbacks
        callbacks; //< Used by the HLS access-out to notify segments events to the sout.

    bool is_caching;
    vlc_fifo_t *cache;

    bool segment_just_ended; //< true if the `SegmentAdded` was just called.

    vlc_tick_t exposed_media_time;
    //vlc_tick_t pcr; //< Latest received PCR from `SetPCR`

    struct vlc_list node;

    const char *type;
};

typedef struct segment_http_context
{
    httpd_url_t *url;
    struct segment_http_context *next;
} segment_http_context;

typedef struct
{
    httpd_host_t *httpd_host;
    httpd_url_t *httpd_main_index_url; //< Main playlist index file URL.

    struct
    {
        hls_io *handle;
        vlc_mutex_t lock;
    } main_index; //< Main playlist index IO handler.

    struct vlc_list medias; //< List all the HLS medias handled by the sout

    int main_media_es_id; //< Main media es id. Chosen by the user.
    struct hls_media *main_media;


    char *fs_path; //< Directory path where all the HLS files should be done. Default is NULL and
                   // segments are kept in memory.

    struct
    {
        unsigned int max_length;
        unsigned int max_count;
    } seg_infos; //< Segments global data.

    vlc_tick_t pcr;
} sout_stream_sys_t;

#define foreach_hls_medias(it) \
    vlc_list_foreach( it, &sys->medias, node )


static inline bool sys_IsLive( const sout_stream_sys_t *sys )
{
    return sys->seg_infos.max_count != 0;
}

/**
 * Allocate an empty block and mark it as HEADER.
 *
 * HEADER blocks have a special meaning in the HLS workflow. They are used to force the termination
 * of HLS Segments at the access level. Basically, when the sout needs a segment to terminate
 * because it has accumulated too much delay in comparison to the others, it sends an empty HEADER
 * block to force the access out to close the segment properly.
 */
static inline block_t *block_AllocEmptyHeader( vlc_tick_t dts )
{
    block_t *header = block_Alloc( 0 );
    if ( unlikely( header == NULL ) )
        return NULL;

    header->i_flags |= BLOCK_FLAG_HEADER;
    header->i_dts = dts;
    header->i_pts = dts;
    return header;
}

/**
 * Callback notifying a new segment.
 *
 * Exposes the segment via HTTP and hold a reference on it in the media structure.
 * Called by the HLS access output.
 */
static int SegmentAdded( void *opaque, const hls_segment *segment )
{
    struct hls_media *media = opaque;

    vlc_url_t url;
    if ( vlc_UrlParse( &url, segment->psz_uri ) != VLC_SUCCESS )
        return VLC_EGENERIC;

    segment_http_context *context = calloc( 1, sizeof( *context ) );
    if ( unlikely( context == NULL ) )
        return VLC_ENOMEM;

    context->url = httpd_UrlNew( media->httpd_host, url.psz_path, NULL, NULL );
    if ( unlikely( context->url == NULL ) )
    {
        free( context );
        vlc_UrlClean( &url );
        return VLC_ENOMEM;
    }
    httpd_UrlCatch( context->url, HTTPD_MSG_GET, hls_http_SegmentCallback, (void *)segment );

    vlc_queue_Enqueue( &media->segment_queue, context );

    media->segment_just_ended = true;

    media->exposed_media_time += segment->segment_length;

    // Ask the main media to cache until next SetPCR call to let all the data from other es's time
    // to arrive
    if ( media->is_main_media )
        media->is_caching = true;

    vlc_UrlClean( &url );
    return VLC_SUCCESS;
}

/**
 * Callback notifying an index playlist change.
 *
 * Called by the HLS access output.
 */
static void IndexUpdated( void *opaque, const hls_io *index_io )
{
    struct hls_media *media = opaque;

    vlc_mutex_lock( &media->index_handler.lock );
    media->index_handler.handle = index_io;
    vlc_mutex_unlock( &media->index_handler.lock );
}

/**
 * Callback notifying segment removal.
 *
 * Called by the HLS access output.
 */
static void SegmentRemoved( void *opaque, const hls_segment *segment )
{
    struct hls_media *obj = opaque;

    segment_http_context *context = vlc_queue_Dequeue( &obj->segment_queue );

    // Should not be called with en empty queue.
    assert( context );

    httpd_UrlDelete( context->url );
    free( context );

    (void)segment;
}

static char *hls_media_FormatURI( const char *fs_path, unsigned id )
{
    char *ret;
    if ( fs_path != NULL )
    {
        if ( unlikely( asprintf( &ret, "%s/track-%u-index.m3u8", fs_path, id ) == -1 ) )
            return NULL;
    }
    else
    {
        if ( unlikely( asprintf( &ret, "/track-%u-index.m3u8", id ) == -1 ) )
            return NULL;
    }
    return ret;
}

static httpd_url_t *hls_media_InitIndexUrl( httpd_host_t *host, const char *fs_path, unsigned id )
{
    char *url = hls_media_FormatURI( fs_path, id );
    if ( unlikely( url == NULL ) )
        return NULL;
    httpd_url_t *ret = httpd_UrlNew( host, url, NULL, NULL );
    free( url );
    return ret;
}

static const char *hls_media_InferAccessName( enum es_format_category_e type )
{
    switch ( type )
    {
    case AUDIO_ES:
        return "pkd-audio-hls";
    case VIDEO_ES: // Fallthrough
    case SPU_ES:
        return "hls";
    default:
        return NULL;
    }
}

static const char *hls_media_InferAccessFileExtension( enum es_format_category_e type )
{
    switch ( type )
    {
    case AUDIO_ES:
        return "aac";
    case VIDEO_ES:
        return "ts";
    case SPU_ES:
        return "vtt";
    default:
        return NULL;
    }
}

static char *hls_media_GenAccessConfigLine( const sout_stream_sys_t *sys, const es_format_t *fmt )
{
    struct vlc_memstream ms;
    if ( unlikely( vlc_memstream_open( &ms ) != VLC_SUCCESS ) )
        return NULL;

    const unsigned int media_index = fmt->i_id;
    const char *ext = hls_media_InferAccessFileExtension( fmt->i_cat );
    const char *aco = hls_media_InferAccessName( fmt->i_cat );

    char *url;
    if ( unlikely( asprintf( &url, "%s/track-%u-########.%s", sys->fs_path, media_index, ext ) ) == -1 )
        return NULL;

    vlc_memstream_printf( &ms, "%s{seglen=%u,numsegs=%u,segment-url=%s", aco,
                          sys->seg_infos.max_length, sys->seg_infos.max_count, url );

    if ( sys->fs_path != NULL )
    {
        char *index_path;
        if ( unlikely( asprintf( &index_path, "%s/track-%u-index.m3u8", sys->fs_path,
                                 media_index ) ) == -1 )
        {
            goto err;
        }

        char *segment_path;
        if ( unlikely( asprintf( &segment_path, "%s/track-%u-########.%s", sys->fs_path,
                                 media_index, ext ) ) == -1 )
        {
            free( index_path );
            goto err;
        }

        vlc_memstream_printf( &ms, ",index-path=%s,segment-path=%s", index_path, segment_path );
        free( index_path );
        free( segment_path );
    }
    else
    {
        vlc_memstream_puts( &ms, ",delsegs" );
    }

    vlc_memstream_putc( &ms, '}' );

    if ( unlikely( vlc_memstream_close( &ms ) != VLC_SUCCESS ) )
        goto err; // TODO what ?

    free( url );
    return ms.ptr;

err:
    if ( likely( vlc_memstream_close( &ms ) == VLC_SUCCESS ) )
        free( ms.ptr );
    free( url );
    return NULL;
}

static sout_mux_t *
get_muxer( sout_access_out_t *access, const sout_stream_sys_t *sys, enum es_format_category_e type )
{
    switch ( type )
    {
    case AUDIO_ES:
        return sout_MuxNew( access, "hlspack" );
    case VIDEO_ES:
        return sout_MuxNew( access, "ts" );
    case SPU_ES:
    {
        char *config;
        if ( unlikely( asprintf( &config, "hlsvtt{seglen=%u}", sys->seg_infos.max_length ) == -1 ) )
            return NULL;
        sout_mux_t *ret = sout_MuxNew( access, config );
        free( config );
        return ret;
    }
    default:
        return NULL;
    }
}

static struct hls_media *hls_media_New( sout_stream_t *sout, const es_format_t *fmt )
{
    assert( fmt->i_id >= 0 );

    struct hls_media *media = calloc( 1, sizeof( *media ) );
    if ( unlikely( media == NULL ) )
        return NULL;

    sout_stream_sys_t *sys = sout->p_sys;

    media->httpd_host = sys->httpd_host;
    media->httpd_index_url = hls_media_InitIndexUrl( media->httpd_host, sys->fs_path, fmt->i_id );
    if ( unlikely( media->httpd_index_url == NULL ) )
    {
        free( media );
        return NULL;
    }

    httpd_UrlCatch( media->httpd_index_url, HTTPD_MSG_GET, hls_http_IndexCallback,
                    (void *)&media->index_handler );

    vlc_queue_Init( &media->segment_queue, offsetof( segment_http_context, next ) );

    vlc_mutex_init( &media->index_handler.lock );

    media->callbacks = ( struct hls_sout_callbacks ){ .sys = media,
                                                      .segment_added = SegmentAdded,
                                                      .segment_removed = SegmentRemoved,
                                                      .index_updated = IndexUpdated };

    sout_access_out_t *access = NULL;
    char *access_config = hls_media_GenAccessConfigLine( sys, fmt );
    if ( access_config != NULL )
    {
        access = sout_AccessOutNew( sout, access_config, "livehttp" );
        free( access_config );
        if ( access == NULL )
            goto err;
    }
    else
        goto err;

    var_Create( access, HLS_SOUT_CALLBACKS_VAR, VLC_VAR_ADDRESS );
    var_SetAddress( access, HLS_SOUT_CALLBACKS_VAR, &media->callbacks );

    media->mux = get_muxer( access, sys, fmt->i_cat );
    if ( unlikely( media->mux == NULL ) )
    {
        goto err;
    }

    media->mux_input = sout_MuxAddStream( media->mux, fmt );
    if ( media->mux_input == NULL )
    {
        goto err;
    }

    media->cache = block_FifoNew();
    if ( unlikely( media->cache == NULL ) )
    {
        goto err;
    }

    media->exposed_media_time = 0;

    es_format_Copy( &media->fmt, fmt );

    media->is_main_media = sys->main_media_es_id == fmt->i_id;

    vlc_list_init( &media->node );

    return media;
err:
    if ( media->mux_input != NULL )
        sout_MuxDeleteStream( media->mux, media->mux_input );
    if ( media->mux != NULL )
        sout_MuxDelete( media->mux );
    if ( access != NULL )
        sout_AccessOutDelete( access );
    if ( media->httpd_index_url != NULL )
        httpd_UrlDelete( media->httpd_index_url );
    free( media );
    return NULL;
}

static void hls_media_Release( struct hls_media *media )
{
    sout_access_out_t *access = media->mux->p_access;

    sout_AccessOutDelete( access );

    sout_MuxDeleteStream( media->mux, media->mux_input );
    sout_MuxDelete( media->mux );

    // All segments should have been cleaned up after the access release.
    assert( vlc_queue_IsEmpty( &media->segment_queue ) );

    httpd_UrlDelete( media->httpd_index_url );

    free( media );
}

/**
 * Drain the media cache until an AddSegment event is caught.
 *
 * @return VLC_EGENERIC if the segment failed to terminate.
 *         VLC_SUCCESS otherwise.
 */
static int
hls_media_TerminateSegment( struct hls_media *media, vlc_tick_t seg_end )
{
    printf( "Terminate %d\n", media->fmt.i_id );
    vlc_fifo_Lock(media->cache);
    while (!vlc_fifo_IsEmpty(media->cache) && !media->segment_just_ended)
    {
        block_t *to_mux = vlc_fifo_DequeueUnlocked(media->cache);
        sout_MuxSendBuffer( media->mux, media->mux_input, to_mux );
    }
    vlc_fifo_Unlock(media->cache);

    if ( media->segment_just_ended )
    {
        media->segment_just_ended = false;
    }
    else
    {
        // All the cache has been emptied and the segment is still not naturally closed by the
        // access out. Send a HEADER block to the pipeline to force segment termination.
        block_t *header = block_AllocEmptyHeader( seg_end );
        if ( likely( header != NULL ) )
        {
            if ( media->fmt.i_cat == VIDEO_ES )
            {
                // XXX: Ugly workaround the fact that the video muxer (ts in this case) discards
                // empty blocks. For now, send the header block directly to the access to avoid
                // that...
                sout_AccessOutWrite( media->mux->p_access, header );
            }
            else
            {
                sout_MuxSendBuffer( media->mux, media->mux_input, header );
            }
        }

        if ( media->segment_just_ended )
        {
            media->segment_just_ended = false;
        }
        else
        {
            return VLC_EGENERIC;
        }
    }

    media->is_caching = false;
    return VLC_SUCCESS;
}

/**
 *  Fill the given memstream with a full HLS index description file content.
 *
 *  @return false if the index can't be generated.
 *          true on success.
 */
static bool sys_FillIndex( const sout_stream_sys_t *sys, struct vlc_memstream *stream )
{
    static const char hls_hdr[] = "#EXTM3U\n";
    vlc_memstream_write( stream, hls_hdr, sizeof( hls_hdr ) - 1 );

    const struct hls_media *default_medias[] = {
        [VIDEO_ES] = NULL,
        [AUDIO_ES] = NULL,
        [SPU_ES] = NULL,
    };

    // Test all current medias to determine which substreams should be marked as default selected.
    const struct hls_media *media;
    foreach_hls_medias(media)
    {
        if ( media->is_main_media )
            continue;

        const struct hls_media *highest = default_medias[media->fmt.i_cat];
        const int current_prio = media->fmt.i_priority;
        if ( highest == NULL || highest->fmt.i_priority < current_prio )
            default_medias[media->fmt.i_cat] = media;
    }

    // Generate an HLS sub-stream media for each secondary medias.
    const struct hls_media *stream_inf = NULL;
    foreach_hls_medias(media)
    {
        // Ignore and keep track of the main media.
        if ( media->is_main_media )
        {
            stream_inf = media;
            continue;
        }

        static const char *const media_types[] = {
            [VIDEO_ES] = "VIDEO",
            [AUDIO_ES] = "AUDIO",
            [SPU_ES] = "SUBTITLES",
        };
        const es_format_t *fmt = &media->fmt;
        const char *media_type = media_types[media->fmt.i_cat];

        char media_name[32];
        sprintf( media_name, "%d", fmt->i_id );

        const char *is_default_media = default_medias[fmt->i_cat] == media ? "YES" : "NO";
        vlc_memstream_printf(
            stream,
            "#EXT-X-MEDIA:TYPE=%s,GROUP-ID=\"%s\",NAME=\"%s\",LANGUAGE=\"en-US\",DEFAULT=%s,",
            media_type, media_type, media_name, is_default_media );

        if ( fmt->psz_language )
            vlc_memstream_printf( stream, "LANGUAGE=\"%s\",", fmt->psz_language );

        char *uri = hls_media_FormatURI( sys->fs_path, fmt->i_id );
        if ( likely( uri != NULL ) )
        {
            vlc_memstream_printf( stream, "URI=\"%s\"\n", uri );
            free( uri );
        }
    }

    // We should have kept track of the main media in the loop above.
    if ( !stream_inf )
    {
        return false;
    }

    const unsigned bandwidth = 0; // TODO: compute bandwidth when we support multiple main streams.
    const char codecs[] = "avc1.42c01e,mp4a.40.2"; // TODO: don't hardcode codec informations.
    vlc_memstream_printf( stream, "#EXT-X-STREAM-INF:BANDWIDTH=%d,CODECS=\"%s\"", bandwidth,
                          codecs );

    // XXX: For the sake of simplicity, only link default sub-medias to the main stream for now.
    if ( default_medias[VIDEO_ES] )
        vlc_memstream_printf( stream, ",VIDEO=\"VIDEO\"" );
    if ( default_medias[AUDIO_ES] )
        vlc_memstream_printf( stream, ",AUDIO=\"AUDIO\"" );
    if ( default_medias[SPU_ES] )
        vlc_memstream_printf( stream, ",SUBTITLES=\"SUBTITLES\"" );

    char *uri = hls_media_FormatURI( sys->fs_path, stream_inf->fmt.i_id );
    vlc_memstream_printf( stream, "\n%s\n", uri );
    free( uri );

    return true;
}

static inline hls_io *sys_CreateIndexIO( const char *fs_path )
{
    hls_io *ret;
    if ( fs_path != NULL )
    {
        char *path;
        if ( unlikely( asprintf( &path, "%s/stream.m3u8", fs_path ) == -1 ) )
            return NULL;
        ret = hls_io_New( path, O_WRONLY | O_CREAT | O_TRUNC );
    }
    else
    {
        ret = hls_io_New( NULL, 0 );
    }
    return ret;
}

static int sys_UpdateIndex( sout_stream_sys_t *sys )
{
    struct vlc_memstream stream;
    if ( vlc_memstream_open( &stream ) != VLC_SUCCESS )
        return VLC_EGENERIC;

    if ( !sys_FillIndex( sys, &stream ) )
    {
        if ( vlc_memstream_close( &stream ) == VLC_SUCCESS )
            free( stream.ptr );
        return VLC_EGENERIC;
    }

    if ( vlc_memstream_close( &stream ) != VLC_SUCCESS )
        return VLC_EGENERIC;

    hls_io *io = sys_CreateIndexIO( sys->fs_path );
    if ( unlikely( io == NULL ) )
    {
        free( stream.ptr );
        return VLC_EGENERIC;
    }

    // Dump the content of the index content in a newly created IO handler.
    if (io->ops.open( io ) != VLC_SUCCESS)
    {
        io->ops.release(io);
        free(io);
        free(stream.ptr);
        return VLC_EGENERIC;
    }
    block_t *block = block_heap_Alloc( stream.ptr, stream.length );
    io->ops.consume_block( io, block );
    io->ops.close( io );

    // Replace the current index IO by the freshly regenerated one.
    vlc_mutex_lock( &sys->main_index.lock );
    hls_io *current_handle = sys->main_index.handle;
    if ( current_handle != NULL )
    {
        current_handle->ops.release( current_handle );
        free( current_handle );
    }
    sys->main_index.handle = io;
    vlc_mutex_unlock( &sys->main_index.lock );

    return VLC_SUCCESS;
}

static void *Add( sout_stream_t *sout, const es_format_t *fmt )
{
    if ( fmt->i_cat == DATA_ES || fmt->i_cat == UNKNOWN_ES ||
         fmt->i_priority == ES_PRIORITY_NOT_SELECTABLE )
    {
        msg_Warn( sout, "Unsupported es." );
        return NULL;
    }

    char a[5] = { 0 };
    char b[5] = { 0 };

    vlc_fourcc_to_char( fmt->i_codec, a );
    vlc_fourcc_to_char( VLC_CODEC_A52, b );
    printf( "CODEC: %s %s\n", a, b );
    if ( fmt->i_cat == AUDIO_ES && fmt->i_codec != VLC_CODEC_MP4A &&
         fmt->i_codec != VLC_CODEC_MPGA && fmt->i_codec != VLC_CODEC_A52 )
    {
        msg_Warn( sout, "Unsupported audio track codec." );
        return NULL;
    }

    static const char * const TYPES[] = {
        [SPU_ES] = "SPU",
        [VIDEO_ES] = "VIDEO",
        [AUDIO_ES] = "AUDIO"
    };
    struct hls_media *media = hls_media_New( sout, fmt );
    if ( media == NULL )
        return NULL;
    media->type = TYPES[fmt->i_cat];

    sout_stream_sys_t *sys = sout->p_sys;

    if ( media->is_main_media || sys->main_media == NULL)
    {
        sys->main_media = media;
    }

    vlc_list_append( &media->node, &sys->medias );

    if ( sys_UpdateIndex( sys ) != VLC_SUCCESS )
    {
        msg_Warn( sout, "Failed to update the HLS main playlist index." );
    }

    return media;
}

static void Del( sout_stream_t *sout, void *id )
{
    sout_stream_sys_t *sys = sout->p_sys;
    struct hls_media *media = id;

   // Make sure media's own cache is drained by terminating the segment.
    if ( hls_media_TerminateSegment( media, sys->pcr ) != VLC_SUCCESS )
    {
        msg_Warn( sout, "Couldn't force segment termination (stream id: %d).", media->fmt.i_id );
    }


    vlc_list_remove( &media->node );

    if ( media->is_main_media )
    {
        sys->main_media = vlc_list_first_entry_or_null( &sys->medias, struct hls_media, node );
    }

    hls_media_Release( media );

    if ( sys_UpdateIndex( sout->p_sys ) != VLC_SUCCESS )
    {
        msg_Warn( sout, "Failed to update the HLS main playlist index." );
    }
}

static int Send( sout_stream_t *stream, void *id, block_t *buffer )
{
    struct hls_media *media = id;
    const sout_stream_sys_t *sys = stream->p_sys;

    struct vlc_tracer *t = vlc_object_get_tracer( VLC_OBJECT( stream ) );
    for ( const block_t *i = buffer; i != NULL; i = i->p_next )
    {
        vlc_tracer_TraceStreamDTS( t, "HLS", media->type, "IN", i->i_pts, i->i_dts );
        if (i->i_flags & BLOCK_FLAG_TYPE_I)
        {
            char *n;
            asprintf(&n, "%s-iframe", media->type);
            vlc_tracer_TraceStreamDTS( t, "HLS", n, "OUT", i->i_pts, i->i_dts );
            free( n );
        }
    }

    // Cache incoming data in all cases if the ES isn't the main one.
    // The main es termination is deciding whenever sub-ESes terminate.
    if ( !media->is_main_media || media->is_caching )
    {
        vlc_fifo_Put(media->cache, buffer);
        return VLC_SUCCESS;
    }

    // In some cases, after segment termination, there is still some leftover blocks belonging to
    // the next segment in the cache. This part mux the first block of the fifo and put the incoming
    // block at the tail of the cache.
    // TODO: This can grow in memory in bad scenarios, this needs some sort of max cache value.
    //if ( !vlc_fifo_IsEmpty( media->cache ) )
    //{
    //    vlc_fifo_Lock( media->cache );
    //    vlc_fifo_QueueUnlocked( media->cache, buffer );
    //    buffer = vlc_fifo_DequeueUnlocked( media->cache );
    //    vlc_fifo_Unlock( media->cache );
    //}

    return sout_MuxSendBuffer( media->mux, media->mux_input, buffer );
}

static void Flush( sout_stream_t *stream, void *id )
{
    sout_stream_sys_t *sys = stream->p_sys;
    struct hls_media *media = id;

    if ( hls_media_TerminateSegment( media, sys->pcr ) != VLC_SUCCESS )
    {
        msg_Warn( stream, "Couldn't force segment termination (stream id: %d).", media->fmt.i_id );
    }

    sout_MuxFlush( media->mux, media->mux_input );

    // TODO: Mux a discontinuity block when the access output supports it.
}

static int Control( sout_stream_t *stream, int query, va_list args )
{
    switch ( query )
    {
    case SOUT_STREAM_IS_SYNCHRONOUS:
        *va_arg( args, bool * ) = true;//sys_IsLive( stream->p_sys );
        break;

    default:
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

/**
 * Called when an ES has a PCR update.
 *
 * PCR update is used to properly end all pending segments when one segment terminated. This is
 * usefull to ensure the sout always exposes segments nearly at the same time and to terminate empty
 * subtitles segments.
 */
static void SetPCR( sout_stream_t *stream, vlc_tick_t pcr )
{
    sout_stream_sys_t *sys = stream->p_sys;

    struct vlc_tracer *t = vlc_object_get_tracer( VLC_OBJECT( stream ) );
    vlc_tracer_TracePCR( t, "HLS", "IN", pcr );

    sys->pcr = pcr;

    if ( sys->main_media == NULL || !sys->main_media->segment_just_ended )
    {
        return;
    }

    struct hls_media *media;
    foreach_hls_medias( media )
    {
        if ( media->is_main_media )
        {
            continue;
        }

        if ( hls_media_TerminateSegment( media, sys->main_media->exposed_media_time ) !=
             VLC_SUCCESS )
        {
            msg_Warn( stream, "Couldn't force segment termination (stream id: %d)",
                      media->fmt.i_id );
        }
    }
    sys->main_media->is_caching = false;
    sys->main_media->segment_just_ended = false;

    vlc_fifo_Lock( sys->main_media->cache );
    while ( !vlc_fifo_IsEmpty( sys->main_media->cache ) )
    {
        block_t *to_mux = vlc_fifo_DequeueUnlocked( sys->main_media->cache );
        Send( stream, sys->main_media, to_mux );

        if ( sys->main_media->segment_just_ended )
        {
            break;
        }
    }
    vlc_fifo_Unlock( sys->main_media->cache );
}

static const struct sout_stream_operations ops = {
    Add, Del, Send, Control, Flush, SetPCR,
};

int SoutOpen( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;

    sout_stream_sys_t *sys = stream->p_sys = calloc( 1, sizeof( *sys ) );
    if ( unlikely( sys == NULL ) )
        return VLC_ENOMEM;

    static const char *const sout_options[] = { "path", "seglen", "segcount", "main-es", NULL };
    config_ChainParse( stream, SOUT_CFG_PREFIX, sout_options, stream->p_cfg );

    sys->main_media_es_id = var_GetInteger( this, SOUT_CFG_PREFIX "main-es" );
    if ( sys->main_media_es_id < 0 )
    {
        msg_Err( this, "Please select a valid es that will represent the main track." );
        goto err;
    }

    sys->httpd_host = vlc_http_HostNew( this );
    if ( unlikely( sys->httpd_host == NULL ) )
        goto err;

    sys->httpd_main_index_url = httpd_UrlNew( sys->httpd_host, "/stream.m3u8", NULL, NULL );
    if ( unlikely( sys->httpd_main_index_url == NULL ) )
        goto err;

    vlc_mutex_init( &sys->main_index.lock );

    vlc_list_init( &sys->medias );

    httpd_UrlCatch( sys->httpd_main_index_url, HTTPD_MSG_GET, hls_http_IndexCallback,
                    (void *)&sys->main_index );

    sys->fs_path = var_GetString( this, SOUT_CFG_PREFIX "path" );
    sys->seg_infos.max_length = var_GetInteger( this, SOUT_CFG_PREFIX "seglen" );
    sys->seg_infos.max_count = var_GetInteger( this, SOUT_CFG_PREFIX "segcount" );
    sys->pcr = VLC_TICK_INVALID;

    stream->ops = &ops;

    return VLC_SUCCESS;
err:
    if ( sys->httpd_host )
        httpd_HostDelete( sys->httpd_host );
    free( sys );
    return VLC_EGENERIC;
}

void SoutClose( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;

    httpd_UrlDelete( sys->httpd_main_index_url );
    httpd_HostDelete( sys->httpd_host );

    free( sys->fs_path );
    free( sys );
}
