/*****************************************************************************
 * pcr-sync.c PCR track sync stream filter
 *****************************************************************************
 * Copyright (C) 2021 VideoLAN Authors
 *
 * Author: Alaric Senat <dev.asenat@posteo.net>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>

#include <vlc_block.h>
#include <vlc_list.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_tracer.h>

#include <assert.h>

typedef struct
{
    struct vlc_list stream_id_sys_list;
    vlc_tick_t max_pcr;
} sout_stream_sys_t;

typedef struct
{
    block_t *last_block_ref;
    block_fifo_t *cache;
    struct vlc_list pcr_events;

    void *next_id;

    bool set_pcr_error;

    // TODO remove
    const char *cat;

    struct vlc_list node;
} sout_stream_id_sys_t;

#define for_each_stream_id( it, head ) vlc_list_foreach( it, head, node )

struct pcr_event
{
    vlc_tick_t pcr;
    block_t *end_of_sequence;
    struct vlc_list node;
};

static void *Add( sout_stream_t *stream, const es_format_t *fmt )
{
    sout_stream_id_sys_t *id = calloc( 1, sizeof( *id ) );
    if ( unlikely( id == NULL ) )
        return NULL;

    id->next_id = sout_StreamIdAdd( stream->p_next, fmt );
    if ( id->next_id == NULL )
    {
        free( id );
        return NULL;
    }

    id->cache = block_FifoNew();
    if ( unlikely( id->cache == NULL ) )
    {
        free( id );
        return NULL;
    }
    vlc_list_init( &id->pcr_events );

    sout_stream_sys_t *sys = stream->p_sys;
    vlc_list_append( &id->node, &sys->stream_id_sys_list );

    return id;
}

static inline block_t *block_ChainGetLast( block_t *chain )
{
    assert( chain != NULL );

    block_t *last = chain;
    while ( last->p_next != NULL )
        last = last->p_next;
    return last;
}

static int Send( sout_stream_t *stream, void *id, block_t *buffer )
{
    sout_stream_id_sys_t *id_sys = id;

    if ( id_sys->set_pcr_error )
        return VLC_EGENERIC;

    struct vlc_tracer *tracer = vlc_object_get_tracer( VLC_OBJECT( stream ) );
    if ( tracer != NULL )
    {
        for ( const block_t *it = buffer; it != NULL; it = it->p_next )
            vlc_tracer_TraceStreamDTS( tracer, "SYNC", id_sys->cat, "IN", it->i_pts, it->i_dts );
    }

    id_sys->last_block_ref = block_ChainGetLast( buffer );

    block_FifoPut( id_sys->cache, buffer );

    return VLC_SUCCESS;

    (void)stream;
}

static bool IsLastToReceivePCR( const struct vlc_list *streams, vlc_tick_t pcr )
{
    const sout_stream_id_sys_t *it;
    for_each_stream_id( it, streams )
    {
        const struct pcr_event *first_pcr_event =
            vlc_list_first_entry_or_null( &it->pcr_events, struct pcr_event, node );

        if ( first_pcr_event == NULL || first_pcr_event->pcr > pcr )
        {
            return false;
        }
    }
    return true;
}

/**
 * Send all the cached blocks that have a dts inferior to the given pcr sequentially to the next
 * stream.
 */
static void
SendFromOneESUntilPCR( sout_stream_t *stream, sout_stream_id_sys_t *es_id, vlc_tick_t pcr )
{
    struct vlc_tracer *tracer = vlc_object_get_tracer( VLC_OBJECT( stream ) );

    while ( !vlc_list_is_empty( &es_id->pcr_events ) )
    {
        struct pcr_event *pcr_event =
            vlc_list_first_entry_or_null( &es_id->pcr_events, struct pcr_event, node );

        if ( pcr_event->pcr <= pcr )
        {
            vlc_fifo_Lock( es_id->cache );

            // Get starting point of cached blocks.
            block_t *to_send = vlc_fifo_DequeueAllUnlocked( es_id->cache );

            // Put the out-of-pcr-sequence data back in the cache.
            if ( pcr_event->end_of_sequence != NULL )
            {
                vlc_fifo_QueueUnlocked( es_id->cache, pcr_event->end_of_sequence->p_next );
                // End the `to_send` block chain right after the sequence.
                pcr_event->end_of_sequence->p_next = NULL;
            }

            vlc_fifo_Unlock( es_id->cache );

            if ( to_send != NULL )
            {
                if ( tracer != NULL )
                {
                    for ( const block_t *it = to_send; it != NULL; it = it->p_next )
                        vlc_tracer_TraceStreamDTS( tracer, "SYNC", es_id->cat, "OUT", it->i_pts,
                                                   it->i_dts );
                }
                sout_StreamIdSend( stream->p_next, es_id->next_id, to_send );
            }

            if ( tracer != NULL )
            {
                vlc_tracer_TracePCR( tracer, "SYNCOUT", es_id->cat, pcr_event->pcr );
            }
            // Forward the pcr value to the next stream
            sout_StreamSetPCR( stream->p_next, es_id->next_id, pcr_event->pcr );

            // Release list entry.
            vlc_list_remove( &pcr_event->node );
            free( pcr_event );
        }
        else
        {
            // Stop uncaching when we reach the pcr fence given in parameters.
            break;
        }
    }
}

static void SendFromAllESUntilPCR( sout_stream_t *stream, vlc_tick_t pcr )
{
    sout_stream_sys_t *sys = stream->p_sys;
    sout_stream_id_sys_t *es_id;
    for_each_stream_id( es_id, &sys->stream_id_sys_list )
    {
        SendFromOneESUntilPCR( stream, es_id, pcr );
    }
}

static inline void DrainEs( sout_stream_t *stream, sout_stream_id_sys_t *id )
{
    const sout_stream_sys_t *sys = stream->p_sys;
    SendFromOneESUntilPCR( stream, id, sys->max_pcr );
}

static void SetPCR( sout_stream_t *stream, void *id, vlc_tick_t pcr )
{
    sout_stream_sys_t *sys = stream->p_sys;
    sout_stream_id_sys_t *id_sys = id;

    struct vlc_tracer *tracer = vlc_object_get_tracer( VLC_OBJECT( stream ) );
    if ( tracer != NULL )
    {
        vlc_tracer_TracePCR( tracer, "SYNCIN", id_sys->cat, pcr );
    }

    struct pcr_event *pcr_event = malloc( sizeof( *pcr_event ) );
    if ( unlikely( pcr_event == NULL ) )
    {
        // The next Send call will report the error.
        id_sys->set_pcr_error = true;
        return;
    }

    pcr_event->pcr = pcr;
    pcr_event->end_of_sequence = id_sys->last_block_ref;
    id_sys->last_block_ref = NULL;

    vlc_list_append( &pcr_event->node, &id_sys->pcr_events );

    if ( IsLastToReceivePCR( &sys->stream_id_sys_list, pcr ) )
    {
        SendFromAllESUntilPCR( stream, pcr );
    }
    sys->max_pcr = __MAX( pcr, sys->max_pcr );
}

static void Del( sout_stream_t *stream, void *id )
{
    sout_stream_id_sys_t *id_sys = id;

    DrainEs( stream, id_sys );

    assert( vlc_list_is_empty( &id_sys->pcr_events ) );

    assert( vlc_fifo_IsEmpty( id_sys->cache ) );
    block_FifoRelease( id_sys->cache );

    vlc_list_remove( &id_sys->node );
    free( id_sys );
}

static int Open( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;

    sout_stream_sys_t *sys = malloc( sizeof( *sys ) );
    if ( unlikely( sys == NULL ) )
        return VLC_ENOMEM;

    vlc_list_init( &sys->stream_id_sys_list );
    sys->max_pcr = VLC_TICK_0;

    static const struct sout_stream_operations ops = {
        .add = Add, .del = Del, .send = Send, .set_pcr = SetPCR };
    stream->ops = &ops;
    stream->p_sys = sys;

    return VLC_SUCCESS;
}

static void Close( vlc_object_t *this )
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;
    assert( vlc_list_is_empty( &sys->stream_id_sys_list ) );

    free( sys );
}

// clang-format off
vlc_module_begin()
    set_shortname( N_("pcr-sync") )
    set_description( N_("Synchronize ES streams with pcr events") )
    set_category( CAT_SOUT )
    set_capability( "sout filter", 0 )
    set_callbacks( Open, Close )
    add_shortcut("pcr-sync")
vlc_module_end()
    // clang-format on
