/*****************************************************************************
 * cc_startup_cache.h: Chromecast HLS startup buffering helper
 *****************************************************************************
 * Copyright © 2024 VideoLAN
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
#ifndef VLC_CC_STARTUP_CACHE_H
#define VLC_CC_STARTUP_CACHE_H

#include <vector>
#include <deque>

#include <vlc_sout.h>
#include <vlc_block.h>
#include <vlc_variables.h>

/* PCR window before the HLS chain is built. Long enough for es_out to have
 * delivered the full initial ES set and a few frames of each track. */
#define CC_STARTUP_CACHING VLC_TICK_FROM_MS(500)

/* Per-track record kept for the lifetime of the cache (Add → Del). */
struct cc_cache_track_t
{
    es_format_t  fmt;
    char        *es_id;         /* strdup of the es_id passed to Add */
    void        *downstream_id; /* sout_StreamIdAdd result; NULL until wired */
    bool         selected;      /* chosen by build_cb to appear in variants= */
};

/* One ordered entry in the pre-roll FIFO. */
struct cc_cache_event_t
{
    bool              is_pcr;
    cc_cache_track_t *track; /* non-NULL for block events */
    block_t          *block; /* owned; non-NULL for block events */
    vlc_tick_t        pcr;  /* for PCR events */
};

struct cc_startup_cache_sys_t;

/*
 * Called once, when the caching window elapses.
 *
 * The callback MUST on success:
 *   - mark chosen tracks (track->selected = true) and fill track->downstream_id
 *     via sout_StreamIdAdd on the new chain
 *   - assign the chain head to cache->p_next
 *   - store the hls-ready var callback in sys->hls_ready_fn / hls_ready_data
 *     (so the cache can remove it on reset)
 *   - replay sys->events in order via sout_StreamId* / sout_StreamSetPCR
 *   - clear sys->events
 *   - set sys->ready = true
 *   - return true
 *
 * On failure the callback MUST:
 *   - delete any partially created chain, clear cache->p_next
 *   - clear track->selected / downstream_id on all tracks
 *   - release and clear sys->events
 *   - return false (the cache marks hls_failed and stops retrying until reset)
 *
 * A failing callback MAY additionally set sys->build_retry, meaning "this
 * configuration did not work but another might". The cache then re-arms the
 * caching window instead of latching hls_failed, and calls the callback again
 * on the next window. The callback is responsible for changing something
 * between attempts (e.g. escalating the transcoding state) so that retrying
 * terminates.
 */
typedef bool (*cc_cache_build_fn)(void *opaque, sout_stream_t *cache,
                                  cc_startup_cache_sys_t *sys);

struct cc_startup_cache_sys_t
{
    cc_cache_build_fn   build_cb;
    void               *build_opaque;

    vlc_tick_t          first_pcr;     /* VLC_TICK_INVALID until first SetPCR */
    bool                ready;         /* false = buffering, true = pass-through */
    bool                hls_failed;    /* build_cb returned false; stop retrying */
    bool                build_retry;   /* build_cb asked to be called again */

    /* Set by build_cb when it registers a readiness var callback; cleared on
     * reset so cc_startup_cache_Reset can remove the callback cleanly. */
    sout_stream_t      *hls_out;
    vlc_callback_t      hls_ready_fn;
    void               *hls_ready_data;

    std::vector<cc_cache_track_t *> tracks;
    std::deque<cc_cache_event_t>    events;
};

/*
 * Create the startup-cache sout_stream_t helper.
 *
 * parent   — the cast sout_stream_t; the cache is created as its VLC child.
 * build_cb — called when the window elapses (see typedef above).
 * opaque   — forwarded verbatim to build_cb.
 *
 * The returned stream has p_next = NULL.  Its ops handle Add/Del/Send/Flush/
 * SetPCR.  Pass it to sout_StreamChainDelete(cache, NULL) to free it.
 */
sout_stream_t *cc_startup_cache_New(sout_stream_t *parent,
                                    cc_cache_build_fn build_cb,
                                    void *opaque);

/*
 * Tear down the live HLS chain (if any), drain and release all buffered
 * events, and reset every track's downstream_id / selected fields so that
 * the next PCR window triggers a fresh build_cb call.
 *
 * Track records themselves (cc_cache_track_t) are NOT freed; they survive
 * across resets because the cast module has not called Del on them.
 *
 * The caller is responsible for any owner-side bookkeeping that must happen
 * alongside a reset (e.g. cc_has_input, requestPlayerStop).
 */
void cc_startup_cache_Reset(sout_stream_t *cache);

#endif /* VLC_CC_STARTUP_CACHE_H */
