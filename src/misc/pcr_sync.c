#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdint.h>

#include <vlc_common.h>

#include <vlc_list.h>
#include <vlc_pcr_sync.h>
#include <vlc_tick.h>
#include <vlc_vector.h>

struct es_data
{
    bool is_deleted;
    vlc_tick_t last_frame_dts;
};
static inline struct es_data es_data_Init()
{
    return ( struct es_data ){ .is_deleted = false, .last_frame_dts = VLC_TICK_INVALID };
}
struct es_data_vec VLC_VECTOR( struct es_data );

struct vlc_pcr_sync
{
    struct vlc_list pcr_events;
    struct es_data_vec es_data;
    vlc_mutex_t lock;
};

struct es_dts_entry
{
    vlc_tick_t dts;
    bool passed;
};
struct es_dts_entry_vec VLC_VECTOR( struct es_dts_entry );

typedef struct
{
    vlc_tick_t pcr;
    struct es_dts_entry_vec es_last_dts_entries;
    unsigned int entries_left;
    bool no_frame_before;

    struct vlc_list node;
} pcr_event_t;

static inline void pcr_event_Delete( pcr_event_t *ev )
{
    vlc_vector_destroy( &ev->es_last_dts_entries );
    free( ev );
}

vlc_pcr_sync_t *vlc_pcr_sync_New( void )
{
    vlc_pcr_sync_t *ret = malloc( sizeof( *ret ) );
    if ( unlikely( ret == NULL ) )
        return NULL;

    vlc_vector_init( &ret->es_data );
    vlc_list_init( &ret->pcr_events );
    vlc_mutex_init( &ret->lock );
    return ret;
}

void vlc_pcr_sync_Delete( vlc_pcr_sync_t *pcr_sync )
{
    vlc_vector_destroy( &pcr_sync->es_data );

    pcr_event_t *e;
    while ( ( e = vlc_list_first_entry_or_null( &pcr_sync->pcr_events, pcr_event_t, node ) ) )
    {
        vlc_list_remove( &e->node );
        free( e );
    }

    free( pcr_sync );
}

static bool pcr_sync_ShouldFastForwardPCR( vlc_pcr_sync_t *pcr_sync )
{
    struct es_data it;
    vlc_vector_foreach( it, &pcr_sync->es_data )
    {
        if ( !it.is_deleted && it.last_frame_dts != VLC_TICK_INVALID )
            return false;
    }
    return vlc_list_is_empty( &pcr_sync->pcr_events );
}

static bool pcr_sync_HadFrameInputSinceLastPCR( vlc_pcr_sync_t *pcr_sync )
{
    const pcr_event_t *pcr_event =
        vlc_list_last_entry_or_null( &pcr_sync->pcr_events, pcr_event_t, node );

    if ( pcr_event == NULL )
        return true;

    for ( unsigned int i = 0; i < pcr_sync->es_data.size; ++i )
    {
        const struct es_data *curr = &pcr_sync->es_data.data[i];
        if ( curr->is_deleted || curr->last_frame_dts == VLC_TICK_INVALID )
            continue;

        const bool is_oob = i >= pcr_event->es_last_dts_entries.size;
        if ( !is_oob && curr->last_frame_dts != pcr_event->es_last_dts_entries.data[i].dts )
            return true;
        else if ( is_oob )
            return true;
    }
    return false;
}

static int pcr_sync_SignalPCRLocked( vlc_pcr_sync_t *pcr_sync, vlc_tick_t pcr )
{
    if ( pcr_sync_ShouldFastForwardPCR( pcr_sync ) )
        return VLC_PCR_SYNC_FORWARD_PCR;

    pcr_event_t *event = malloc( sizeof( *event ) );
    if ( unlikely( event == NULL ) )
        return VLC_ENOMEM;

    vlc_vector_init( &event->es_last_dts_entries );
    const bool alloc_succeeded =
        vlc_vector_reserve( &event->es_last_dts_entries, pcr_sync->es_data.size );
    if ( unlikely( alloc_succeeded == false ) )
    {
        free( event );
        return VLC_ENOMEM;
    }

    unsigned int entries_left = 0;
    struct es_data data;
    vlc_vector_foreach( data, &pcr_sync->es_data )
    {
        if ( !data.is_deleted )
        {
            vlc_vector_push( &event->es_last_dts_entries,
                             ( struct es_dts_entry ){ .dts = data.last_frame_dts } );
            if ( data.last_frame_dts != VLC_TICK_INVALID )
            {
                ++entries_left;
            }
        }
    }

    event->pcr = pcr;
    event->entries_left = entries_left;
    event->no_frame_before = !pcr_sync_HadFrameInputSinceLastPCR( pcr_sync );

    vlc_list_append( &event->node, &pcr_sync->pcr_events );

    return VLC_SUCCESS;
}

int vlc_pcr_sync_SignalPCR( vlc_pcr_sync_t *pcr_sync, vlc_tick_t pcr )
{
    vlc_mutex_lock( &pcr_sync->lock );
    const int result = pcr_sync_SignalPCRLocked( pcr_sync, pcr );
    vlc_mutex_unlock( &pcr_sync->lock );
    return result;
}

void vlc_pcr_sync_SignalFrame( vlc_pcr_sync_t *pcr_sync, unsigned int id, vlc_tick_t frame_dts )
{
    vlc_mutex_lock( &pcr_sync->lock );
    assert( id < pcr_sync->es_data.size );

    struct es_data *data = &pcr_sync->es_data.data[id];
    assert( !data->is_deleted );
    data->last_frame_dts = frame_dts;

    vlc_mutex_unlock( &pcr_sync->lock );
}

int vlc_pcr_sync_NewESID( vlc_pcr_sync_t *pcr_sync, unsigned int *id )
{
    vlc_mutex_lock( &pcr_sync->lock );

    for ( unsigned int i = 0; i < pcr_sync->es_data.size; ++i )
    {
        if ( pcr_sync->es_data.data[i].is_deleted )
        {
            pcr_sync->es_data.data[i] = es_data_Init();
            vlc_mutex_unlock( &pcr_sync->lock );
            *id = i;
            return VLC_SUCCESS;
        }
    }

    const bool push_succeeded = vlc_vector_push( &pcr_sync->es_data, es_data_Init() );
    const size_t ids_count = pcr_sync->es_data.size;

    vlc_mutex_unlock( &pcr_sync->lock );

    if ( push_succeeded )
    {
        *id = ids_count - 1;
        return VLC_SUCCESS;
    }
    return VLC_ENOMEM;
}

void vlc_pcr_sync_DelESID( vlc_pcr_sync_t *pcr_sync, unsigned int id )
{
    vlc_mutex_lock( &pcr_sync->lock );
    pcr_sync->es_data.data[id].is_deleted = true;
    vlc_mutex_unlock( &pcr_sync->lock );
}

static inline void pcr_sync_ResetLastReceivedFrames( vlc_pcr_sync_t *pcr_sync )
{
    for ( size_t i = 0; i < pcr_sync->es_data.size; ++i )
    {
        pcr_sync->es_data.data[i].last_frame_dts = VLC_TICK_INVALID;
    }
}

vlc_tick_t
vlc_pcr_sync_SignalFrameOutput( vlc_pcr_sync_t *pcr_sync, unsigned int id, vlc_tick_t frame_dts )
{
    vlc_mutex_lock( &pcr_sync->lock );

    pcr_event_t *pcr_event =
        vlc_list_first_entry_or_null( &pcr_sync->pcr_events, pcr_event_t, node );

    if ( pcr_event == NULL )
    {
        goto no_pcr;
    }

    assert( id < pcr_event->es_last_dts_entries.size );

    const vlc_tick_t pcr = pcr_event->pcr;

    if ( pcr_event->no_frame_before )
    {
        goto return_pcr;
    }

    assert( pcr_event->entries_left != 0 );
    const vlc_tick_t last_dts = pcr_event->es_last_dts_entries.data[id].dts;
    if ( last_dts == VLC_TICK_INVALID )
    {
        pcr_event_t *it;
        vlc_list_foreach( it, &pcr_sync->pcr_events, node )
        {
            if ( it == pcr_event )
                continue;

            struct es_dts_entry *entry = &it->es_last_dts_entries.data[id];
            if ( entry->dts == VLC_TICK_INVALID || entry->passed )
            {
                continue;
            }
            if ( frame_dts < entry->dts )
            {
                break;
            }

            entry->passed = true;
            --it->entries_left;
            assert( it->entries_left != 0 );
        }
        goto no_pcr;
    }

    if ( frame_dts < last_dts )
    {
        goto no_pcr;
    }

    pcr_event->es_last_dts_entries.data[id].passed = true;
    if ( --pcr_event->entries_left != 0 )
    {
        goto no_pcr;
    }

return_pcr:
    vlc_list_remove( &pcr_event->node );
    pcr_event_Delete( pcr_event );

    pcr_sync_ResetLastReceivedFrames( pcr_sync );
    vlc_mutex_unlock( &pcr_sync->lock );
    return pcr;

no_pcr:
    vlc_mutex_unlock( &pcr_sync->lock );
    return VLC_TICK_INVALID;
}
