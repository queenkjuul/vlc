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

struct vlc_pcr_delayer
{
    vlc_tick_t delay;

    vlc_tick_t media_time_elapsed;
    vlc_tick_t last_block_end;
    vlc_tick_t last_dts;

    struct vlc_list delayed_blocks_data;
    struct vlc_list pcr_events;

    vlc_mutex_t lock;
};

typedef struct
{
    vlc_tick_t pcr;
    vlc_tick_t last_dts;
    struct vlc_list node;
} pcr_event_t;

typedef struct
{
    vlc_tick_t media_time;
    vlc_tick_t dts;
    pcr_event_t *attached_following_pcr;
    struct vlc_list node;
} delayed_block_data_t;

int vlc_pcr_delayer_SignalPCR( vlc_pcr_delayer_t *delayer, vlc_tick_t pcr, vlc_tick_t block_dts )
{
    // Some streams with unpredictable data flow can have big "data holes" between pcr events
    // (typically SPU streams). In the case the latest block DTS didn't change since last call, we
    // tell the user to immediately forward the pcr event.
    vlc_mutex_lock( &delayer->lock );
    const vlc_tick_t last_dts = delayer->last_dts;
    delayer->last_dts = block_dts;
    if ( last_dts == VLC_TICK_INVALID || last_dts == block_dts )
    {
        if ( vlc_list_is_empty( &delayer->pcr_events ) )
        {
            vlc_mutex_unlock( &delayer->lock );
            return VLC_EGENERIC; // TODO Find a more suitable error code
        }
    }
    vlc_mutex_unlock( &delayer->lock );

    pcr_event_t *event = calloc( 1, sizeof( *event ) );
    if ( unlikely( event == NULL ) )
        return VLC_ENOMEM;

    event->last_dts = block_dts;
    event->pcr = pcr;

    vlc_mutex_lock( &delayer->lock );
    vlc_list_append( &event->node, &delayer->pcr_events );
    vlc_mutex_unlock( &delayer->lock );

    return VLC_SUCCESS;
}

static pcr_event_t *pcr_event_FindWithDTS( struct vlc_list *pcr_events, vlc_tick_t dts )
{
    pcr_event_t *ret;
    vlc_list_foreach( ret, pcr_events, node )
    {
        if ( ret->last_dts == dts )
            return ret;
    }
    return NULL;
}
//       +---------------------------------+
// ----> | black box with media time delay | ----->
//   ^   +---------------------------------+
//   L here
int vlc_pcr_delayer_SignalEnteringBlock( vlc_pcr_delayer_t *delayer, const block_t *block )
{
    delayed_block_data_t *node = malloc( sizeof( *node ) );
    if ( unlikely( node == NULL ) )
        return VLC_ENOMEM;

    vlc_mutex_lock( &delayer->lock );

    node->media_time = ( block->i_dts - delayer->last_block_end ) + block->i_length;
    delayer->media_time_elapsed += node->media_time;
    delayer->last_block_end = block->i_dts;

    node->attached_following_pcr = pcr_event_FindWithDTS( &delayer->pcr_events, block->i_dts );

    vlc_list_append( &node->node, &delayer->delayed_blocks_data );

    // Something went wrong in the delayed block processing unit.
    // exceeding this limit usually means the block was dropped. So in our case, act like it went
    // through.
    if ( delayer->media_time_elapsed > delayer->delay )
    {
        vlc_pcr_delayer_SignalLeavingBlock( delayer, block );
        // TODO forward the setpcr call. maybe return a specific error code so the user do so.
        vlc_mutex_unlock( &delayer->lock );
        return VLC_EGENERIC;
    }

    vlc_mutex_unlock( &delayer->lock );
    return VLC_SUCCESS;
}

//       +---------------------------------+
// ----> | black box with media time delay | ----->
//       +---------------------------------+  ^
//                                            L here
int vlc_pcr_delayer_SignalLeavingBlock( vlc_pcr_delayer_t *delayer, const block_t *block )
{
    vlc_mutex_lock( &delayer->lock );

    delayed_block_data_t *block_data =
        vlc_list_first_entry_or_null( &delayer->delayed_blocks_data, delayed_block_data_t, node );
    if ( block_data == NULL )
    {
        vlc_mutex_unlock( &delayer->lock );
        return VLC_EGENERIC;
    }

    if ( block_data->attached_following_pcr != NULL )
    {
        pcr_event_t *pcr_event =
            vlc_list_first_entry_or_null( &delayer->pcr_events, pcr_event_t, node );
        // If the delayed block changed the timestamp to be higher than the former PCR, fix the pcr
        // value accordingly.
        pcr_event->pcr = __MAX( block->i_dts, pcr_event->pcr );
        pcr_event->last_dts = block->i_dts;
    }

    delayer->media_time_elapsed -= block_data->media_time;
    vlc_list_remove( &block_data->node );
    free( block_data );

    vlc_mutex_unlock( &delayer->lock );
    return VLC_SUCCESS;
}

block_t *vlc_pcr_delayer_BindPCRToDTS( vlc_pcr_delayer_t *delayer,
                                       struct vlc_pcr_delayer_pair *out,
                                       block_t *blocks )
{
    pcr_event_t *pcr_event =
        vlc_list_first_entry_or_null( &delayer->pcr_events, pcr_event_t, node );
    if ( pcr_event == NULL )
    {
        goto no_pcr_matched;
    }

    block_t *prev = NULL;
    for ( block_t *it = blocks; it != NULL; it = it->p_next )
    {
        if ( it->i_dts >= pcr_event->last_dts )
        {
            out->pcr = pcr_event->pcr;
            out->blocks = blocks;

            block_t *ret;
            if ( prev != NULL )
            {
                prev->p_next = NULL;
                ret = it;
            }
            else
            {
                ret = it->p_next;
                it->p_next = NULL;
            }

            vlc_list_remove( &pcr_event->node );
            free( pcr_event );
            return ret;
        }
        prev = it;
    }

no_pcr_matched:
    out->blocks = blocks;
    out->pcr = VLC_TICK_INVALID;
    return NULL;
}

vlc_pcr_delayer_t *vlc_pcr_delayer_New( vlc_tick_t delay )
{
    vlc_pcr_delayer_t *delayer = malloc( sizeof( *delayer ) );
    if ( unlikely( delayer == NULL ) )
        return NULL;

    delayer->delay = delay;
    delayer->media_time_elapsed = VLC_TICK_INVALID;
    delayer->last_block_end = VLC_TICK_INVALID;
    delayer->last_dts = VLC_TICK_INVALID;
    vlc_list_init( &delayer->pcr_events );
    vlc_list_init( &delayer->delayed_blocks_data );
    vlc_mutex_init( &delayer->lock );

    return delayer;
}

void vlc_pcr_delayer_Release( vlc_pcr_delayer_t *delayer )
{
    assert( vlc_list_is_empty( &delayer->pcr_events ) );
    assert( vlc_list_is_empty( &delayer->delayed_blocks_data ) );

    free( delayer );
}
