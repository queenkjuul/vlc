/*****************************************************************************
 * pcr_delayer.c:
 *****************************************************************************
 * Copyright (C) 2021 VLC authors and VideoLAN
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

#include <assert.h>
#include <stdint.h>

#include <vlc_common.h>

#include <vlc_block.h>
#include <vlc_list.h>
#include <vlc_pcr_delayer.h>

#include <stdio.h>

struct vlc_pcr_delayer
{
    vlc_tick_t max_delay;

    vlc_tick_t media_time_elapsed;
    vlc_tick_t last_block_end;
    vlc_tick_t last_dts;

    // The last DTS output by `vlc_pcr_delayer_BindPCRToDTS`
    vlc_tick_t last_bound_dts;

    struct vlc_list delayed_blocks_data;
    struct vlc_list pcr_events;

    block_t *output;
    block_t **output_end;

    int ( *send_blocks )( void *, block_t * );
    void ( *send_pcr )( void *, vlc_tick_t );

    vlc_mutex_t lock;
};

typedef struct
{
    vlc_tick_t pcr;
    vlc_tick_t max_dts;

    const block_t *last_block;

    struct vlc_list node;
} pcr_event_t;

typedef struct
{
    vlc_tick_t media_time;
    vlc_tick_t dts;
    pcr_event_t *attached_following_pcr;
    struct vlc_list node;
} delayed_block_data_t;

static inline void pcr_delayer_Lock( vlc_pcr_delayer_t *delayer )
{
    vlc_mutex_lock( &delayer->lock );
}
static inline void pcr_delayer_Unlock( vlc_pcr_delayer_t *delayer )
{
    vlc_mutex_unlock( &delayer->lock );
}

static inline void pcr_delayer_ResetOutput( vlc_pcr_delayer_t *delayer )
{
    delayer->output = NULL;
    delayer->output_end = &delayer->output;
}

// B1 B2 P3
// B200 B202 P3
//

// static delayed_block_data_t *delayed_block_data_FindWithDTS( struct vlc_list *delayed_block_data,
//                                                              vlc_tick_t dts )
//{
//     delayed_block_data_t *ret;
//     vlc_list_foreach( ret, delayed_block_data, node )
//     {
//         if ( ret->dts == dts )
//             return ret;
//     }
//     return NULL;
// }
vlc_pcr_delayer_status_t
vlc_pcr_delayer_SignalPCR( vlc_pcr_delayer_t *delayer, vlc_tick_t pcr, vlc_tick_t block_dts )
{
    pcr_delayer_Lock( delayer );

    // Some streams with unpredictable data flow can have big "data holes" between pcr events
    // (typically SPU streams). In the case the latest block DTS didn't change since last call, we
    // tell the user to immediately forward the pcr event.
    const bool is_pcr_delayed =
        delayer->last_dts != block_dts && !vlc_list_is_empty( &delayer->pcr_events );

    delayer->last_dts = block_dts;

    pcr_event_t *event = malloc( sizeof( *event ) );
    if ( unlikely( event == NULL ) )
        return VLC_PCR_DELAYER_ERROR;

    event->max_dts = block_dts;
    event->pcr = pcr;
    event->last_block = NULL;
    event->node = ( struct vlc_list ){ 0 };

    vlc_list_append( &event->node, &delayer->pcr_events );

    delayed_block_data_t *block_data =
        vlc_list_last_entry_or_null( &delayer->delayed_blocks_data, delayed_block_data_t, node );
    if ( block_data != NULL && block_data->dts == block_dts )
    {
        block_data->attached_following_pcr = event;
    }

    pcr_delayer_Unlock( delayer );

    return is_pcr_delayed ? VLC_PCR_DELAYER_SUCCESS : VLC_PCR_DELAYER_NO_DELAY;
}

static vlc_pcr_delayer_status_t pcr_delayer_SignalLeavingBlockUnlocked( vlc_pcr_delayer_t *delayer,
                                                                        block_t *block )
{
    delayed_block_data_t *block_data =
        vlc_list_first_entry_or_null( &delayer->delayed_blocks_data, delayed_block_data_t, node );

    assert( block_data != NULL );

    if ( block_data->attached_following_pcr != NULL )
    {
        pcr_event_t *next_pcr =
            vlc_list_first_entry_or_null( &delayer->pcr_events, pcr_event_t, node );
        // If the delayed block changed the timestamp to be higher than the former PCR, fix the pcr
        // value accordingly.
        if ( next_pcr != NULL )
        {
            next_pcr->pcr = block->i_dts;
            next_pcr->max_dts = block->i_dts;
            next_pcr->last_block = block;
        }
    }

    delayer->media_time_elapsed -= block_data->media_time;

    vlc_list_remove( &block_data->node );
    free( block_data );

    block_ChainLastAppend( &delayer->output_end, block );

    return VLC_PCR_DELAYER_SUCCESS;
}

vlc_pcr_delayer_status_t vlc_pcr_delayer_SignalLeavingBlock( vlc_pcr_delayer_t *delayer,
                                                             block_t *block )
{
    pcr_delayer_Lock( delayer );
    const vlc_pcr_delayer_status_t ret = pcr_delayer_SignalLeavingBlockUnlocked( delayer, block );
    pcr_delayer_Unlock( delayer );
    return ret;
}

static pcr_event_t *pcr_event_FindWithDTS( struct vlc_list *pcr_events, vlc_tick_t dts )
{
    pcr_event_t *ret;
    vlc_list_foreach( ret, pcr_events, node )
    {
        if ( ret->max_dts == dts )
            return ret;
    }
    return NULL;
}

static vlc_pcr_delayer_status_t pcr_delayer_SignalEnteringBlockUnlocked( vlc_pcr_delayer_t *delayer,
                                                                         const block_t *block )
{
    delayed_block_data_t *bdata = malloc( sizeof( *bdata ) );
    if ( unlikely( bdata == NULL ) )
        return VLC_PCR_DELAYER_ERROR;

    bdata->media_time = ( block->i_dts - delayer->last_block_end ) + block->i_length;
    bdata->dts = block->i_dts;
    bdata->attached_following_pcr = pcr_event_FindWithDTS( &delayer->pcr_events, block->i_dts );

    delayer->media_time_elapsed += bdata->media_time;
    delayer->last_block_end = block->i_dts;

    vlc_list_append( &bdata->node, &delayer->delayed_blocks_data );

    vlc_pcr_delayer_status_t status = VLC_PCR_DELAYER_SUCCESS;
    // Something went wrong in the delaying unit.
    // Exceeding this limit usually means the block was dropped. So in our case, act like it went
    // through.
    // TODO needs to be properly unit-tested.
    if ( delayer->media_time_elapsed > delayer->max_delay )
    {
        delayed_block_data_t *first_bdata = vlc_list_first_entry_or_null(
            &delayer->delayed_blocks_data, delayed_block_data_t, node );
        assert( first_bdata != NULL );

        if ( first_bdata->attached_following_pcr != NULL )
        {
            delayer->last_bound_dts = first_bdata->dts;
            status = VLC_PCR_DELAYER_NO_DELAY;
        }

        vlc_list_remove( &first_bdata->node );
        free( first_bdata );
    }
    return status;
}

vlc_pcr_delayer_status_t vlc_pcr_delayer_SignalEnteringBlock( vlc_pcr_delayer_t *delayer,
                                                              const block_t *block )
{

    pcr_delayer_Lock( delayer );
    const vlc_pcr_delayer_status_t ret = pcr_delayer_SignalEnteringBlockUnlocked( delayer, block );
    pcr_delayer_Unlock( delayer );
    return ret;
}

/**
 * `vlc_pcr_delayer_BindPCRToDTS` output data.
 */
struct vlc_pcr_delayer_pair
{
    block_t *blocks; /**< blockchain start */
    vlc_tick_t pcr;  /**< PCR value if any. VLC_TICK_INVALID if none. */
};

static inline bool pcr_delayer_pair_IsEmpty( const struct vlc_pcr_delayer_pair *pair )
{
    return pair->blocks == NULL && pair->pcr == VLC_TICK_INVALID;
}

/**
 * Extract blocks before the next recorded pcr event.
 *
 * \param out Block chain extract with pcr value bound to it.
 * \param blocks Block chain input
 *
 */
static struct vlc_pcr_delayer_pair pcr_delayer_BindPCRToDTS( vlc_pcr_delayer_t *delayer, bool treat_blocks )
{
    block_t *out = treat_blocks ? delayer->output : NULL;

    pcr_event_t *pcr_event =
        vlc_list_first_entry_or_null( &delayer->pcr_events, pcr_event_t, node );
    if ( pcr_event == NULL )
    {
        pcr_delayer_ResetOutput( delayer );
        return ( struct vlc_pcr_delayer_pair ){ .blocks = out, .pcr = VLC_TICK_INVALID };
    }

    if ( pcr_event->max_dts == delayer->last_bound_dts )
    {
        struct vlc_pcr_delayer_pair ret = { .blocks = NULL, .pcr = pcr_event->pcr };
        vlc_list_remove( &pcr_event->node );
        free( pcr_event );
        return ret;
    }

    struct vlc_pcr_delayer_pair ret = {.blocks = out, .pcr = VLC_TICK_INVALID};
    block_t *prev = NULL;
    for ( block_t *it = out; it != NULL; it = it->p_next )
    {
        if ( it == pcr_event->last_block )
        {
            ret.pcr = pcr_event->pcr;

            block_t *new_output;
            if ( prev != NULL )
            {
                prev->p_next = NULL;
                new_output = it;
            }
            else
            {
                new_output = it->p_next;
                it->p_next = NULL;
            }

            delayer->last_bound_dts = pcr_event->max_dts;
            vlc_list_remove( &pcr_event->node );
            free( pcr_event );
            delayer->output = new_output;
            return ret;
        }
        prev = it;
    }
    pcr_delayer_ResetOutput( delayer );
    return ret;
}

int vlc_pcr_delayer_SendBlocksAndPCR( vlc_pcr_delayer_t *delayer, void *cb_data )
{
    pcr_delayer_Lock( delayer );
    bool is_empty = false;
    while ( !is_empty )
    {
        struct vlc_pcr_delayer_pair out = pcr_delayer_BindPCRToDTS( delayer, true );
        is_empty = pcr_delayer_pair_IsEmpty( &out );

        if ( out.blocks != NULL )
        {
            const int status = delayer->send_blocks( cb_data, out.blocks );
            if ( status != VLC_SUCCESS )
            {
                pcr_delayer_Unlock( delayer );
                return status;
            }
        }

        if ( out.pcr != VLC_TICK_INVALID )
        {
            delayer->send_pcr( cb_data, out.pcr );
        }
    }
    pcr_delayer_Unlock( delayer );

    return VLC_SUCCESS;
}

void vlc_pcr_delayer_SendFrontPCRs( vlc_pcr_delayer_t *delayer, void *cb_data )
{
    pcr_delayer_Lock( delayer );
    bool is_empty = false;
    while ( !is_empty )
    {
        struct vlc_pcr_delayer_pair out = pcr_delayer_BindPCRToDTS( delayer, false );
        is_empty = pcr_delayer_pair_IsEmpty( &out );

        assert( out.blocks == NULL );

        if ( out.pcr != VLC_TICK_INVALID )
        {
            delayer->send_pcr( cb_data, out.pcr );
        }
    }
    pcr_delayer_Unlock( delayer );
}

void vlc_pcr_delayer_Drain( vlc_pcr_delayer_t *delayer, void *cb_data )
{
    delayed_block_data_t *bdata;
    while ( ( bdata = vlc_list_first_entry_or_null( &delayer->delayed_blocks_data,
                                                    delayed_block_data_t, node ) ) )
    {
        if ( bdata->attached_following_pcr )
        {
            delayer->send_pcr(
                cb_data, __MAX( bdata->attached_following_pcr->pcr, delayer->last_bound_dts ) );
        }

        vlc_list_remove( &bdata->node );
        free( bdata );
    }
}

vlc_pcr_delayer_t *vlc_pcr_delayer_New( vlc_tick_t max_delay,
                                        int ( *send_blocks )( void *, block_t * ),
                                        void ( *send_pcr )( void *, vlc_tick_t ) )
{
    vlc_pcr_delayer_t *delayer = malloc( sizeof( *delayer ) );
    if ( unlikely( delayer == NULL ) )
        return NULL;

    delayer->max_delay = max_delay;
    delayer->media_time_elapsed = VLC_TICK_INVALID;
    delayer->last_block_end = VLC_TICK_INVALID;
    delayer->last_dts = VLC_TICK_INVALID;
    delayer->last_bound_dts = VLC_TICK_INVALID;
    delayer->output = NULL;
    delayer->output_end = &delayer->output;
    vlc_list_init( &delayer->pcr_events );
    vlc_list_init( &delayer->delayed_blocks_data );
    delayer->send_blocks = send_blocks;
    delayer->send_pcr = send_pcr;
    vlc_mutex_init( &delayer->lock );

    return delayer;
}

void vlc_pcr_delayer_Delete( vlc_pcr_delayer_t *delayer )
{
    delayed_block_data_t *bdata;
    while ( ( bdata = vlc_list_first_entry_or_null( &delayer->delayed_blocks_data,
                                                    delayed_block_data_t, node ) ) )
    {
        vlc_list_remove( &bdata->node );
        free( bdata );
    }

    pcr_event_t *e;
    while ( ( e = vlc_list_first_entry_or_null( &delayer->pcr_events, pcr_event_t, node ) ) )
    {
        vlc_list_remove( &e->node );
        free( e );
    }

    free( delayer );
}
