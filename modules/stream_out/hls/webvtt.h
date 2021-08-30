#ifndef HLS_WEBVTT_H
#define HLS_WEBVTT_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>

#ifdef __cplusplus
extern "C"
{
#endif

#include <vlc_block.h>
#include <vlc_tick.h>

struct segment
{
    block_t *begin;
    block_t **end;
    vlc_tick_t length;
};

typedef struct sout_mux_sys_t sout_mux_sys_t;
struct sout_mux_sys_t
{
    bool stream_added;

    struct segment segment;

    vlc_tick_t segment_max_length;
    vlc_tick_t last_block_end;
    vlc_tick_t last_segment_end;

    block_t *last_block_ref; //< Reference on the latest block of the segment.

    sout_access_out_t *access;

    ssize_t ( *access_write_fn )(
        sout_mux_sys_t *,
        block_t * ); //< callback for access write, needed to allow mocking for simpler unit tests.
};

int sys_ProcessBlocks( sout_mux_sys_t *sys, block_fifo_t *blocks );
int sys_FlushSegment( sout_mux_sys_t *sys );

#ifdef __cplusplus
}
#endif
#endif
