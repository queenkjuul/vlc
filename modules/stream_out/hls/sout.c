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

struct hls_track
{
    es_format_t fmt;
    sout_mux_t *mux;
    sout_input_t *mux_input;

    bool is_main_track;

    httpd_url_t *httpd_index_url; //<
    hls_index index_handler;      //<

    httpd_host_t *httpd_host;  //< Reference on the http host held in sys.
    vlc_queue_t segment_queue; //< Holds exposed segments.

    struct hls_sout_callbacks
        callbacks; //< Used by the HLS access-out to notify segments events to the sout.

    bool is_caching;
    vlc_fifo_t *cache;

    bool segment_just_ended; //< true if the `SegmentAdded` was just called.

    vlc_tick_t exposed_track_time;

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
    bool is_httpd_host_inherited;
    httpd_url_t *httpd_main_index_url; //< Main playlist index file URL.

    struct
    {
        hls_io *handle;
        vlc_mutex_t lock;
    } main_index; //< Main playlist index IO handler.

    struct vlc_list tracks; //< List all the HLS tracks handled by the sout

    //int main_track_es_id; //< Main track es id. Chosen by the user.
    struct hls_track *main_track;


    char *path; //< Directory path where all the HLS files should be done. Default is NULL and
                // segments are kept in memory.
    bool is_http;

    struct
    {
        unsigned int max_length;
        unsigned int max_count;
    } seg_infos; //< Segments global data.

    unsigned int seg_count;

    vlc_tick_t pcr;

    struct vlc_logger *logger;
} sout_stream_sys_t;

#define foreach_hls_tracks(it) \
    vlc_list_foreach( it, &sys->tracks, node )


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
 * Exposes the segment via HTTP and hold a reference on it in the track structure.
 * Called by the HLS access output.
 */
static int SegmentAdded( void *opaque, const hls_segment *segment )
{
    struct hls_track *track = opaque;

    printf("URI %s\n", segment->psz_uri);
    vlc_url_t url;
    if ( vlc_UrlParse( &url, segment->psz_uri ) != VLC_SUCCESS )
        return VLC_EGENERIC;

    segment_http_context *context = calloc( 1, sizeof( *context ) );
    if ( unlikely( context == NULL ) )
        return VLC_ENOMEM;

    context->url = httpd_UrlNew( track->httpd_host, url.psz_path, NULL, NULL );
    if ( unlikely( context->url == NULL ) )
    {
        free( context );
        vlc_UrlClean( &url );
        return VLC_ENOMEM;
    }
    httpd_UrlCatch( context->url, HTTPD_MSG_GET, hls_http_SegmentCallback, (void *)segment );

    vlc_queue_Enqueue( &track->segment_queue, context );

    track->segment_just_ended = true;

    track->exposed_track_time += segment->segment_length;

    // Ask the main track to cache until next SetPCR call to let all the data from other es's time
    // to arrive
    if ( track->is_main_track )
        track->is_caching = true;

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
    struct hls_track *track = opaque;

    vlc_mutex_lock( &track->index_handler.lock );
    track->index_handler.handle = index_io;
    vlc_mutex_unlock( &track->index_handler.lock );
}

/**
 * Callback notifying segment removal.
 *
 * Called by the HLS access output.
 */
static void SegmentRemoved( void *opaque, const hls_segment *segment )
{
    struct hls_track *obj = opaque;

    segment_http_context *context = vlc_queue_Dequeue( &obj->segment_queue );

    // Should not be called with en empty queue.
    assert( context );

    httpd_UrlDelete( context->url );
    free( context );

    (void)segment;
}

static char *hls_track_FormatURI( const char *fs_path, unsigned id )
{
    char *ret;
    if ( unlikely( asprintf( &ret, "%s/track-%u-index.m3u8", fs_path, id ) == -1 ) )
        return NULL;
    return ret;
}

static httpd_url_t *hls_track_InitIndexUrl( httpd_host_t *host, const char *fs_path, unsigned id )
{
    char *url = hls_track_FormatURI( fs_path, id );
    if ( unlikely( url == NULL ) )
        return NULL;
    printf("SEG URI %s\n", url);
    httpd_url_t *ret = httpd_UrlNew( host, url, NULL, NULL );
    free( url );
    return ret;
}

static const char *hls_track_InferAccessName( enum es_format_category_e type )
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

static const char *hls_track_InferAccessFileExtension( enum es_format_category_e type )
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

static char *hls_track_GenAccessConfigLine( const sout_stream_sys_t *sys, const es_format_t *fmt )
{
    struct vlc_memstream ms;
    if ( unlikely( vlc_memstream_open( &ms ) != VLC_SUCCESS ) )
        return NULL;

    const unsigned int track_index = fmt->i_id;
    const char *ext = hls_track_InferAccessFileExtension( fmt->i_cat );
    const char *aco = hls_track_InferAccessName( fmt->i_cat );

    char *url;
    if ( unlikely( asprintf( &url, "%s/track-%u-########.%s", sys->path, track_index, ext ) ) == -1 )
        return NULL;

    vlc_memstream_printf( &ms, "%s{seglen=%u,numsegs=%u,segment-url=%s", aco,
                          sys->seg_infos.max_length, sys->seg_infos.max_count, url );

    char *index_path;
    if ( unlikely( asprintf( &index_path, "%s/track-%u-index.m3u8", sys->path, track_index ) ) ==
         -1 )
    {
        goto err;
    }

    char *segment_path;
    if ( unlikely( asprintf( &segment_path, "%s/track-%u-########.%s", sys->path, track_index,
                             ext ) ) == -1 )
    {
        free( index_path );
        goto err;
    }

    if (!sys->is_http)
    {
        vlc_memstream_printf( &ms, ",segment-path=%s,index-path=%s", segment_path, index_path );
    }
    vlc_memstream_printf( &ms, ",segment-url=%s", segment_path );
    free( index_path );
    free( segment_path );

    if ( sys->path[0] == '\0' )
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

static struct hls_track *hls_track_New( sout_stream_t *sout, const es_format_t *fmt )
{
    assert( fmt->i_id >= 0 );

    struct hls_track *track = calloc( 1, sizeof( *track ) );
    if ( unlikely( track == NULL ) )
        return NULL;

    sout_stream_sys_t *sys = sout->p_sys;

    track->httpd_host = sys->httpd_host;
    track->httpd_index_url = hls_track_InitIndexUrl( track->httpd_host, sys->path, fmt->i_id );
    if ( unlikely( track->httpd_index_url == NULL ) )
    {
        free( track );
        return NULL;
    }

    httpd_UrlCatch( track->httpd_index_url, HTTPD_MSG_GET, hls_http_IndexCallback,
                    (void *)&track->index_handler );

    vlc_queue_Init( &track->segment_queue, offsetof( segment_http_context, next ) );

    vlc_mutex_init( &track->index_handler.lock );

    track->callbacks = ( struct hls_sout_callbacks ){ .sys = track,
                                                      .segment_added = SegmentAdded,
                                                      .segment_removed = SegmentRemoved,
                                                      .index_updated = IndexUpdated };

    sout_access_out_t *access = NULL;
    char *access_config = hls_track_GenAccessConfigLine( sys, fmt );
    if ( access_config == NULL )
        goto err;

    access = sout_AccessOutNew( sout, access_config, "livehttp" );
    free( access_config );
    if ( access == NULL )
        goto err;

    var_Create( access, HLS_SOUT_CALLBACKS_VAR, VLC_VAR_ADDRESS );
    var_SetAddress( access, HLS_SOUT_CALLBACKS_VAR, &track->callbacks );

    track->mux = get_muxer( access, sys, fmt->i_cat );
    if ( unlikely( track->mux == NULL ) )
    {
        goto err;
    }

    track->mux_input = sout_MuxAddStream( track->mux, fmt );
    if ( track->mux_input == NULL )
    {
        goto err;
    }

    track->cache = block_FifoNew();
    if ( unlikely( track->cache == NULL ) )
    {
        goto err;
    }

    track->exposed_track_time = 0;

    es_format_Copy( &track->fmt, fmt );

    //track->is_main_track = sys->main_track_es_id == fmt->i_id;

    vlc_list_init( &track->node );

    return track;
err:
    if ( track->mux_input != NULL )
        sout_MuxDeleteStream( track->mux, track->mux_input );
    if ( track->mux != NULL )
        sout_MuxDelete( track->mux );
    if ( access != NULL )
        sout_AccessOutDelete( access );
    if ( track->httpd_index_url != NULL )
        httpd_UrlDelete( track->httpd_index_url );
    free( track );
    return NULL;
}

static void hls_track_Release( struct hls_track *track )
{
    sout_access_out_t *access = track->mux->p_access;

    sout_AccessOutDelete( access );

    sout_MuxDeleteStream( track->mux, track->mux_input );
    sout_MuxDelete( track->mux );

    es_format_Clean(&track->fmt);

    // All segments should have been cleaned up after the access release.
    assert( vlc_queue_IsEmpty( &track->segment_queue ) );

    httpd_UrlDelete( track->httpd_index_url );

    vlc_fifo_Delete( track->cache );

    free( track );
}

/**
 * Drain the track cache until an AddSegment event is caught.
 *
 * @return VLC_EGENERIC if the segment failed to terminate.
 *         VLC_SUCCESS otherwise.
 */
static int
hls_track_TerminateSegment( struct hls_track *track, vlc_tick_t seg_end )
{
    printf( "Terminate %d\n", track->fmt.i_id );
    vlc_fifo_Lock(track->cache);
    while (!vlc_fifo_IsEmpty(track->cache) && !track->segment_just_ended)
    {
        block_t *to_mux = vlc_fifo_DequeueUnlocked(track->cache);
        sout_MuxSendBuffer( track->mux, track->mux_input, to_mux );
    }
    vlc_fifo_Unlock(track->cache);

    if ( track->segment_just_ended )
    {
        track->segment_just_ended = false;
    }
    else
    {
        // All the cache has been emptied and the segment is still not naturally closed by the
        // access out. Send a HEADER block to the pipeline to force segment termination.
        block_t *header = block_AllocEmptyHeader( seg_end );
        if ( likely( header != NULL ) )
        {
            if ( track->fmt.i_cat == VIDEO_ES )
            {
                // XXX: Ugly workaround the fact that the video muxer (ts in this case) discards
                // empty blocks. For now, send the header block directly to the access to avoid
                // that...
                sout_AccessOutWrite( track->mux->p_access, header );
            }
            else
            {
                sout_MuxSendBuffer( track->mux, track->mux_input, header );
            }
        }

        if ( track->segment_just_ended )
        {
            track->segment_just_ended = false;
        }
        else
        {
            return VLC_EGENERIC;
        }
    }

    track->is_caching = false;
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

    const struct hls_track *default_tracks[] = {
        [VIDEO_ES] = NULL,
        [AUDIO_ES] = NULL,
        [SPU_ES] = NULL,
    };

    // Test all current tracks to determine which substreams should be marked as default selected.
    const struct hls_track *track;
    foreach_hls_tracks(track)
    {
        if ( track->is_main_track )
            continue;

        const struct hls_track *highest = default_tracks[track->fmt.i_cat];
        const int current_prio = track->fmt.i_priority;
        if ( highest == NULL || highest->fmt.i_priority < current_prio )
            default_tracks[track->fmt.i_cat] = track;
    }

    // Generate an HLS sub-stream track for each secondary tracks.
    const struct hls_track *stream_inf = NULL;
    foreach_hls_tracks(track)
    {
        // Ignore and keep track of the main track.
        if ( track->is_main_track )
        {
            stream_inf = track;
            continue;
        }

        static const char *const track_types[] = {
            [VIDEO_ES] = "VIDEO",
            [AUDIO_ES] = "AUDIO",
            [SPU_ES] = "SUBTITLES",
        };
        const es_format_t *fmt = &track->fmt;
        const char *track_type = track_types[track->fmt.i_cat];

        char track_name[32];
        sprintf( track_name, "%d", fmt->i_id );

        const char *is_default_track = default_tracks[fmt->i_cat] == track ? "YES" : "NO";
        vlc_memstream_printf(
            stream,
            "#EXT-X-MEDIA:TYPE=%s,GROUP-ID=\"%s\",NAME=\"%s\",DEFAULT=%s,AUTOSELECT=YES,FORCED=YES,",
            track_type, track_type, track_name, is_default_track );

        if ( fmt->psz_language )
            vlc_memstream_printf( stream, "LANGUAGE=\"%s\",", fmt->psz_language );
        else
            vlc_memstream_printf( stream, "LANGUAGE=\"eng\"," );

        char *uri = hls_track_FormatURI( sys->path, fmt->i_id );
        if ( likely( uri != NULL ) )
        {
            vlc_memstream_printf( stream, "URI=\"%s\"\n", uri );
            free( uri );
        }
    }

    // We should have kept track of the main track in the loop above.
    if ( !stream_inf )
    {
        return false;
    }

    const unsigned bandwidth = 0; // TODO: compute bandwidth when we support multiple main streams.
    const char codecs[] = "avc1.4D401F,mp4a.40.2,wvtt"; // TODO: don't hardcode codec informations.
    vlc_memstream_printf( stream, "#EXT-X-STREAM-INF:BANDWIDTH=%d,CODECS=\"%s\"", bandwidth,
                          codecs );

    // XXX: For the sake of simplicity, only link default sub-tracks to the main stream for now.
    if ( default_tracks[VIDEO_ES] )
        vlc_memstream_printf( stream, ",VIDEO=\"VIDEO\"" );
    if ( default_tracks[AUDIO_ES] )
        vlc_memstream_printf( stream, ",AUDIO=\"AUDIO\"" );
    if ( default_tracks[SPU_ES] )
        vlc_memstream_printf( stream, ",SUBTITLES=\"SUBTITLES\"" );

    char *uri = hls_track_FormatURI( sys->path, stream_inf->fmt.i_id );
    vlc_memstream_printf( stream, "\n%s", uri );
    free( uri );

    return true;
}


static inline hls_io *sys_CreateIndexIO( const char *path )
{
    hls_io *ret;
    if ( path != NULL )
    {
        char *index;
        if ( unlikely( asprintf( &index, "%s/stream.m3u8", path ) == -1 ) )
            return NULL;
        printf("PATH: %s\n", index);
        ret = hls_io_New( index, O_WRONLY | O_CREAT | O_TRUNC );
        free(index);
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

    hls_io *io = sys_CreateIndexIO( sys->is_http ? NULL : sys->path );
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
    vlc_debug(sys->logger, "Main playlist update:\n%s", stream.ptr);
    
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
    struct hls_track *track = hls_track_New( sout, fmt );
    if ( track == NULL )
        return NULL;
    track->type = TYPES[fmt->i_cat];

    sout_stream_sys_t *sys = sout->p_sys;

    if ( sys->main_track == NULL)
    {
        if ( fmt->i_cat == AUDIO_ES || fmt->i_cat == VIDEO_ES )
        {
            sys->main_track = track;
            track->is_main_track = true;
        }
    }
    else if ( sys->main_track->fmt.i_cat != VIDEO_ES && fmt->i_cat == VIDEO_ES )
    {
        sys->main_track->is_main_track = false;
        if (sys->main_track->is_caching)
        sys->main_track = track;
    }

    vlc_list_append( &track->node, &sys->tracks );

    if ( sys_UpdateIndex( sys ) != VLC_SUCCESS )
    {
        msg_Dbg( sout, "Failed to update the HLS main playlist index." );
    }

    return track;
}

static void Del( sout_stream_t *sout, void *id )
{
    sout_stream_sys_t *sys = sout->p_sys;
    struct hls_track *track = id;

   // Make sure track's own cache is drained by terminating the segment.
    if ( hls_track_TerminateSegment( track, sys->pcr ) != VLC_SUCCESS )
    {
        msg_Warn( sout, "Couldn't force segment termination (stream id: %d).", track->fmt.i_id );
    }


    vlc_list_remove( &track->node );

    if ( track->is_main_track )
    {
        struct hls_track *new_main_track = NULL;
        struct hls_track *other_track;
        foreach_hls_tracks(other_track)
        {
            if ( new_main_track == NULL &&
                 ( other_track->fmt.i_cat == AUDIO_ES || other_track->fmt.i_cat == VIDEO_ES ) )
            {
                new_main_track = other_track;
            }
            else if ( new_main_track != NULL && new_main_track->fmt.i_cat != VIDEO_ES &&
                      other_track->fmt.i_cat == VIDEO_ES )
            {
                new_main_track = other_track;
            }
        }
        sys->main_track = new_main_track;
    }

    hls_track_Release( track );

    if ( sys_UpdateIndex( sout->p_sys ) != VLC_SUCCESS )
    {
        msg_Warn( sout, "Failed to update the HLS main playlist index." );
    }
}

static int Send( sout_stream_t *stream, void *id, block_t *buffer )
{
    struct hls_track *track = id;
    const sout_stream_sys_t *sys = stream->p_sys;

    struct vlc_tracer *tracer = vlc_object_get_tracer( VLC_OBJECT( stream ) );
    if ( tracer != NULL )
    {
        for ( const block_t *i = buffer; i != NULL; i = i->p_next )
        {
            vlc_tracer_TraceStreamDTS( tracer, "HLS", track->type, "IN", i->i_pts, i->i_dts );
            if ( i->i_flags & BLOCK_FLAG_TYPE_I )
            {
                char *n;
                asprintf( &n, "%s-iframe", track->type );
                vlc_tracer_TraceStreamDTS( tracer, "HLS", n, "OUT", i->i_pts, i->i_dts );
                free( n );
            }
        }
    }

    // Cache incoming data in all cases if the ES isn't the main one.
    // The main es termination is deciding whenever sub-ESes terminate.
    if ( !track->is_main_track || track->is_caching )
    {
        vlc_fifo_Put(track->cache, buffer);
        return VLC_SUCCESS;
    }

    // In some cases, after segment termination, there is still some leftover blocks belonging to
    // the next segment in the cache. This part mux the first block of the fifo and put the incoming
    // block at the tail of the cache.
    // TODO: This can grow in memory in bad scenarios, this needs some sort of max cache value.
    //if ( !vlc_fifo_IsEmpty( track->cache ) )
    //{
    //    vlc_fifo_Lock( track->cache );
    //    vlc_fifo_QueueUnlocked( track->cache, buffer );
    //    buffer = vlc_fifo_DequeueUnlocked( track->cache );
    //    vlc_fifo_Unlock( track->cache );
    //}

    return sout_MuxSendBuffer( track->mux, track->mux_input, buffer );
}

static void Flush( sout_stream_t *stream, void *id )
{
    sout_stream_sys_t *sys = stream->p_sys;
    struct hls_track *track = id;

    if ( hls_track_TerminateSegment( track, sys->pcr ) != VLC_SUCCESS )
    {
        msg_Warn( stream, "Couldn't force segment termination (stream id: %d).", track->fmt.i_id );
    }

    sout_MuxFlush( track->mux, track->mux_input );

    // TODO: Mux a discontinuity block when the access output supports it.
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
    if ( t != NULL )
        vlc_tracer_TracePCR( t, "HLS", "IN", pcr );

    sys->pcr = pcr;

    if ( sys->main_track == NULL || !sys->main_track->segment_just_ended )
    {
        return;
    }

    struct hls_track *track;
    foreach_hls_tracks( track )
    {
        if ( track->is_main_track )
        {
            continue;
        }

        if ( hls_track_TerminateSegment( track, sys->main_track->exposed_track_time ) !=
             VLC_SUCCESS )
        {
            msg_Warn( stream, "Couldn't force segment termination (stream id: %d)",
                      track->fmt.i_id );
        }
    }
    sys->main_track->is_caching = false;
    sys->main_track->segment_just_ended = false;

    vlc_fifo_Lock( sys->main_track->cache );
    while ( !vlc_fifo_IsEmpty( sys->main_track->cache ) )
    {
        block_t *to_mux = vlc_fifo_DequeueUnlocked( sys->main_track->cache );
        Send( stream, sys->main_track, to_mux );

        if ( sys->main_track->segment_just_ended )
        {
            break;
        }
    }
    vlc_fifo_Unlock( sys->main_track->cache );

    var_IncInteger( stream, "seg-count" );
}

static const struct sout_stream_operations ops = {
    Add, Del, Send, NULL, Flush, SetPCR,
};

int SoutOpen( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;

    sout_stream_sys_t *sys = stream->p_sys = calloc( 1, sizeof( *sys ) );
    if ( unlikely( sys == NULL ) )
        return VLC_ENOMEM;

    static const char *const sout_options[] = { "path", "is-http", "seglen", "segcount", "main-es", NULL };
    config_ChainParse( stream, SOUT_CFG_PREFIX, sout_options, stream->p_cfg );

    // sys->main_track_es_id = var_GetInteger( this, SOUT_CFG_PREFIX "main-es" );
    //if ( sys->main_track_es_id < 0 )
    //{
    //    msg_Err( this, "Please select a valid es that will represent the main track." );
    //    goto err;
    //}

    sys->httpd_host = var_InheritAddress( this, "httpd-host" );
    sys->is_httpd_host_inherited = sys->httpd_host == NULL ? false : true;
    if ( !sys->is_httpd_host_inherited )
        sys->httpd_host = vlc_http_HostNew( this );
    if ( unlikely( sys->httpd_host == NULL ) )
        goto err;

    sys->path = var_GetString( this, SOUT_CFG_PREFIX "path" );
    sys->is_http = var_GetBool( this, SOUT_CFG_PREFIX "is-http" );;
    sys->seg_infos.max_length = var_GetInteger( this, SOUT_CFG_PREFIX "seglen" );
    //FIXME rename
    sys->seg_infos.max_count = var_GetInteger( this, SOUT_CFG_PREFIX "segcount" );
    sys->pcr = VLC_TICK_INVALID;
    sys->seg_count = 0;
    sys->logger = this->logger;

    char *path;
    if ( unlikely( asprintf( &path, "%s/stream.m3u8", sys->path ) == -1 ) )
        goto err;
    printf("PATH %s\n", path);
    sys->httpd_main_index_url = httpd_UrlNew( sys->httpd_host, path, NULL, NULL );
    free(path);

    if ( unlikely( sys->httpd_main_index_url == NULL ) )
        goto err;

    vlc_mutex_init( &sys->main_index.lock );

    vlc_list_init( &sys->tracks );

    var_Create( this, "seg-count", VLC_VAR_INTEGER );
    httpd_UrlCatch( sys->httpd_main_index_url, HTTPD_MSG_GET, hls_http_IndexCallback,
                    (void *)&sys->main_index );

    stream->ops = &ops;

    return VLC_SUCCESS;
err:
    if ( !sys->is_httpd_host_inherited && sys->httpd_host != NULL )
        httpd_HostDelete( sys->httpd_host );
    if ( sys->path )
        free( sys->path );
    free( sys );
    return VLC_EGENERIC;
}

void SoutClose( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;

    httpd_UrlDelete( sys->httpd_main_index_url );
    if ( !sys->is_httpd_host_inherited )
        httpd_HostDelete( sys->httpd_host );

    hls_io *io = sys->main_index.handle;
    if (io != NULL)
    {
        //io->ops.close(io);
        io->ops.release(io);
        free(io);
    }

    free( sys->path );
    free( sys );
}
