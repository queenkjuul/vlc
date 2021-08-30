#include "webvtt.h"
#include "common.h"

#include <vlc_memstream.h>
#include <vlc_sout.h>

static inline void segment_Reset( struct segment *segment )
{
    segment->begin = NULL;
    segment->end = &segment->begin;
    segment->length = VLC_TICK_INVALID;
}

static inline void webvtt_DecodeTime( struct vlc_memstream *stream, vlc_tick_t time )
{
    const vlc_tick_t secs = SEC_FROM_VLC_TICK( time );
    vlc_memstream_printf( stream, "%02u:%02u:%02u.%03u", (unsigned)( secs / 3600 ),
                          (unsigned)( secs % 3600 / 60 ), (unsigned)( secs % 60 ),
                          (unsigned)( time % CLOCK_FREQ ) / 1000 );
}

static block_t *webvtt_Generate( block_t *data )
{
    struct vlc_memstream stream;
    if ( vlc_memstream_open( &stream ) != VLC_SUCCESS )
    {
        goto err;
    }

    webvtt_DecodeTime( &stream, data->i_dts - VLC_TICK_0 );
    vlc_memstream_puts( &stream, " --> " );
    webvtt_DecodeTime( &stream, data->i_dts - VLC_TICK_0 + data->i_length );
    vlc_memstream_putc( &stream, '\n' );

    vlc_memstream_puts_len( &stream, (char *)data->p_buffer, data->i_buffer - 1 );
    vlc_memstream_puts( &stream, "\n\n" );

    if ( vlc_memstream_close( &stream ) != VLC_SUCCESS )
    {
        goto err;
    }

    data = block_Realloc( data, 0, stream.length );
    memcpy( data->p_buffer, stream.ptr, stream.length );

    return data;
err:
    block_Release( data );
    return NULL;
}

static void webvtt_FillGapWithEmptySegments( sout_mux_sys_t *sys, const block_t *peek )
{
    const vlc_tick_t last_block_gap = peek->i_dts - sys->last_block_end;
    if ( last_block_gap > sys->segment_max_length )
    {
        // If the gap between the current block and the last muxed block is superior to a full
        // segment length; Flush as many empty segments needed to fill the gap.
        for ( int i = 0; i < last_block_gap / sys->segment_max_length; ++i )
        {
            sys_FlushSegment( sys );
            sys->last_segment_end += sys->segment_max_length;
        }
        // Then prepend what's left of empty data to the current flushed segment.
        sys->segment.length = peek->i_dts - sys->last_segment_end;
    }
    else
    {
        // If the gap is small enough so the sub might fit in the current segment. add the gap
        // to the current segment length.
        sys->segment.length += last_block_gap;
    }
    sys->last_block_end = peek->i_dts + peek->i_length;
}

static void webvtt_SplitLastBLockAndFlush( sout_mux_sys_t *sys, const block_t *peek )
{
    block_t *last = block_Duplicate( sys->last_block_ref );

    // Extra time not fitting in the current segment.
    const vlc_tick_t extra_time = sys->last_block_end - peek->i_dts;

    // Shorten the last block length so it fits the short segment.
    sys->last_block_ref->i_length -= extra_time;

    last->i_dts += sys->last_block_ref->i_length;
    last->i_length = extra_time;

    sys_FlushSegment( sys );
    sys->segment.length = last->i_length;

    block_ChainLastAppend( &sys->segment.end, last );
    sys->last_block_ref = last;
}

static int sys_WriteVTTHeader( sout_mux_sys_t *sys )
{
    char *text_vtt_hdr = strdup( "WEBVTT\n\n" );
    if ( unlikely( text_vtt_hdr == NULL ) )
    {
        return VLC_ENOMEM;
    }

    block_t *header = block_heap_Alloc( text_vtt_hdr, strlen( text_vtt_hdr ) );
    if ( unlikely( header == NULL ) )
    {
        free( text_vtt_hdr );
        return VLC_ENOMEM;
    }

    sys->access_write_fn( sys, header );

    return VLC_SUCCESS;
}

int sys_FlushSegment( sout_mux_sys_t *sys )
{
    const int status = sys_WriteVTTHeader( sys );
    if ( status != VLC_SUCCESS )
    {
        return status;
    }

    for ( block_t *block = sys->segment.begin; block != NULL; )
    {
        block_t *vtt = webvtt_Generate( block );
        if ( unlikely( vtt == NULL ) )
        {
            return VLC_EGENERIC;
        }

        block = vtt->p_next;

        vtt->p_next = NULL;
        sys->access_write_fn( sys, vtt );
    }

    block_t *end = block_Alloc( 0 );
    if ( unlikely( end == NULL ) )
    {
        return VLC_ENOMEM;
    }

    end->i_flags |= BLOCK_FLAG_HEADER;
    sys->access_write_fn( sys, end );

    segment_Reset( &sys->segment );

    sys->last_block_ref = NULL;

    return VLC_SUCCESS;
}

/*
 * HLS webvtt segmentation algorithm.
 *
 * Split incomming subs into segments and create empty WEBVTT segments when no sub is available.
 */
int sys_ProcessBlocks( sout_mux_sys_t *sys, block_fifo_t *blocks )
{
    vlc_fifo_Lock( blocks );
    const size_t block_count = vlc_fifo_GetCount( blocks );
    vlc_fifo_Unlock( blocks );

    for ( size_t i = 0; i < block_count; )
    {
        // First we only take a look at the fifo's first block. We might not dequeue it if the block
        // need to be split between two segments.
        block_t *peek = block_FifoShow( blocks );

        if ( peek->i_flags & BLOCK_FLAG_HEADER )
        {
            if ( sys->last_block_ref && sys->last_block_end > peek->i_dts )
            {
                webvtt_SplitLastBLockAndFlush( sys, peek );
            }
            else
            {
                sys_FlushSegment( sys );
            }
            sys->last_segment_end = peek->i_dts;
            sys->last_block_end = peek->i_dts;

            block_t *hdr = block_FifoGet( blocks );
            block_Release( hdr );

            ++i;
            continue;
        }

        // Make sure the subs segment list is continuous. This creates empty
        // segments when no subs was to be displayed.
        webvtt_FillGapWithEmptySegments( sys, peek );

        const vlc_tick_t segment_end = sys->last_segment_end + sys->segment_max_length;
        if ( peek->i_dts >= segment_end )
        {
            // If the current block doesn't start in the range of the current segment, flush it to
            // start the next one.
            sys_FlushSegment( sys );
            sys->last_segment_end += sys->segment_max_length;
            // Take the offset of the actual beginning of the sub block into account in the new
            // segment length.
            sys->segment.length = peek->i_dts - segment_end;
        }
        else if ( sys->segment.length + peek->i_length > sys->segment_max_length )
        {
            // If the sub block overlaps between segments, we have to split it.

            block_t *dup = block_Duplicate( peek );

            // Extra time not fitting in the current segment.
            const vlc_tick_t extra_time =
                ( sys->segment.length + peek->i_length ) - sys->segment_max_length;

            // Shorten the copy length so it fits the current segment.
            dup->i_length -= extra_time;
            dup->p_next = NULL;

            // Shift timestamps of the block still in the fifo, it will be treated in the next loop
            // iteration.
            // XXX: This might break thread safety...
            peek->i_dts += dup->i_length;
            peek->i_length = extra_time;

            block_ChainLastAppend( &sys->segment.end, dup );
            sys_FlushSegment( sys );
            sys->last_segment_end += sys->segment_max_length;

            // Since this was computed earlier in the loop with the current block timestamps, we
            // need to shorten this value too.
            sys->last_block_end -= extra_time;

            // Restart the loop without iterating the counter. We didn't take ownership of the block
            // in the fifo. Subsequents loop iterations will.
            continue;
        }

        // Finally take ownership of the leading block of the fifo.
        block_t *data = block_FifoGet( blocks );
        if ( data->i_flags & BLOCK_FLAG_HEADER )
        {
            // Ignore HEADER blocks, they are used in the hls sout to trigger the muxing of empty
            // segments at the right time to synchronize the spu track with the main track.
            block_Release( data );
        }
        else
        {
            block_ChainLastAppend( &sys->segment.end, data );
            sys->segment.length += data->i_length;

            sys->last_block_ref = data;
        }
        ++i;
    }
    return VLC_SUCCESS;
}

static int Mux( sout_mux_t *mux )
{
    sout_mux_sys_t *sys = mux->p_sys;

    if ( !sys->stream_added )
    {
        return VLC_EGENERIC;
    }

    sout_input_t *input = mux->pp_inputs[0];

    sys_ProcessBlocks( sys, input->p_fifo );

    return VLC_SUCCESS;
}

static int Control( sout_mux_t *mux, int query, va_list args )
{
    switch ( query )
    {
    case MUX_CAN_ADD_STREAM_WHILE_MUXING:
        *va_arg( args, bool * ) = false;
        return VLC_SUCCESS;

    case MUX_GET_MIME:
        *va_arg( args, char ** ) = strdup( "text/vtt" );
        return VLC_SUCCESS;

    default:
        return VLC_EGENERIC;
    }

    (void)mux;
}

static int AddStream( sout_mux_t *mux, sout_input_t *input )
{
    sout_mux_sys_t *sys = mux->p_sys;

    if ( sys->stream_added || input->p_fmt->i_cat != SPU_ES )
    {
        return VLC_EGENERIC;
    }

    sys->stream_added = true;

    return VLC_SUCCESS;
}

static void DelStream( sout_mux_t *mux, sout_input_t *input )
{
    sout_mux_sys_t *sys = mux->p_sys;

    sys->stream_added = false;

    (void)input;
}

static ssize_t sys_AccessWrite( sout_mux_sys_t *sys, block_t *block )
{
    return sout_AccessOutWrite( sys->access, block );
}

int WebVTTMuxOpen( vlc_object_t *this )
{
    sout_mux_t *mux = (sout_mux_t *)this;

    sout_mux_sys_t *sys = calloc( 1, sizeof( *sys ) );
    if ( unlikely( sys == NULL ) )
    {
        return VLC_ENOMEM;
    }

    sys->segment.end = &sys->segment.begin;

    static const char *const mux_options[] = { "seglen", NULL };
    config_ChainParse( mux, VTT_MUX_CFG_PREFIX, mux_options, mux->p_cfg );

    sys->segment_max_length =
        VLC_TICK_FROM_SEC( var_GetInteger( mux, VTT_MUX_CFG_PREFIX "seglen" ) );

    sys->last_block_end = VLC_TICK_0;
    sys->last_segment_end = VLC_TICK_0;

    sys->access = mux->p_access;
    sys->access_write_fn = sys_AccessWrite;

    mux->p_sys = sys;

    mux->pf_control = Control;
    mux->pf_addstream = AddStream;
    mux->pf_delstream = DelStream;
    mux->pf_mux = Mux;

    return VLC_SUCCESS;
}

void WebVTTMuxClose( vlc_object_t *this )
{
    sout_mux_t *mux = (sout_mux_t *)this;

    free( mux->p_sys );
}
