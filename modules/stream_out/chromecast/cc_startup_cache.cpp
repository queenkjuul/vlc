/*****************************************************************************
 * cc_startup_cache.cpp: Chromecast HLS startup buffering helper
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

/*
 * The startup cache is an internal sout_stream_t (not a registered module)
 * that sits at the head of the Chromecast sub-chain.  While buffering it
 * records every Add/Send/SetPCR call into its track list and event FIFO.
 * Once CC_STARTUP_CACHING worth of PCR has been received the build_cb is
 * invoked: it selects tracks, creates the HLS sout chain, wires the tracks
 * into it, replays the FIFO, and marks the cache as ready.  Subsequent
 * calls are forwarded transparently through cache->p_next (the HLS chain).
 *
 * Mid-stream track changes and seek flushes are handled by the cast module
 * calling cc_startup_cache_Reset(), which tears down the live chain and
 * returns the cache to its initial buffering state.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cc_startup_cache.h"

#include <algorithm>

#include <vlc_common.h>

/*****************************************************************************
 * Internal helpers
 *****************************************************************************/

static cc_startup_cache_sys_t *CacheSys(sout_stream_t *stream)
{
    return static_cast<cc_startup_cache_sys_t *>(stream->p_sys);
}

/* Release all buffered events without replaying them. */
static void DropEvents(cc_startup_cache_sys_t *sys)
{
    for (auto &ev : sys->events)
        if (!ev.is_pcr && ev.block)
            block_ChainRelease(ev.block);
    sys->events.clear();
}

/* Undo any downstream wiring established by the build_cb and destroy the
 * live HLS chain.  Leaves cache->p_next == NULL. */
static void TearDownChain(sout_stream_t *cache)
{
    cc_startup_cache_sys_t *sys = CacheSys(cache);

    if (cache->p_next == nullptr)
        return;

    msg_Dbg(cache, "tearing down HLS chain");

    if (sys->hls_ready_fn)
        var_DelCallback(sys->hls_out, "hls-ready",
                        sys->hls_ready_fn, sys->hls_ready_data);

    for (auto *t : sys->tracks)
        if (t->downstream_id)
            sout_StreamIdDel(cache->p_next, t->downstream_id);

    sout_StreamChainDelete(cache->p_next, nullptr);
    cache->p_next      = nullptr;
    sys->hls_out       = nullptr;
    sys->hls_ready_fn  = nullptr;
    sys->hls_ready_data = nullptr;
}

/*****************************************************************************
 * sout_stream_operations callbacks
 *****************************************************************************/

static void *CacheAdd(sout_stream_t *stream,
                      const es_format_t *fmt, const char *es_id)
{
    cc_startup_cache_sys_t *sys = CacheSys(stream);

    cc_cache_track_t *track = new (std::nothrow) cc_cache_track_t();
    if (unlikely(!track))
        return nullptr;

    if (es_format_Copy(&track->fmt, fmt) != VLC_SUCCESS)
    {
        delete track;
        return nullptr;
    }
    track->es_id         = strdup(es_id);
    track->downstream_id = nullptr;
    track->selected      = false;

    if (unlikely(!track->es_id))
    {
        es_format_Clean(&track->fmt);
        delete track;
        return nullptr;
    }

    msg_Dbg(stream, "buffering track %s (codec %4.4s, cat %d)",
            es_id, (const char *)&fmt->i_codec, fmt->i_cat);

    sys->tracks.push_back(track);
    return track;
}

static void CacheDel(sout_stream_t *stream, void *id)
{
    cc_startup_cache_sys_t *sys = CacheSys(stream);
    cc_cache_track_t *track = static_cast<cc_cache_track_t *>(id);

    /* Release any blocks buffered for this track. */
    for (auto it = sys->events.begin(); it != sys->events.end(); )
    {
        if (!it->is_pcr && it->track == track)
        {
            block_ChainRelease(it->block);
            it = sys->events.erase(it);
        }
        else
            ++it;
    }

    if (track->downstream_id && stream->p_next)
        sout_StreamIdDel(stream->p_next, track->downstream_id);

    auto it = std::find(sys->tracks.begin(), sys->tracks.end(), track);
    if (it != sys->tracks.end())
        sys->tracks.erase(it);

    es_format_Clean(&track->fmt);
    free(track->es_id);
    delete track;
}

static int CacheSend(sout_stream_t *stream, void *id, block_t *block)
{
    cc_startup_cache_sys_t *sys = CacheSys(stream);
    cc_cache_track_t *track = static_cast<cc_cache_track_t *>(id);

    if (sys->ready)
    {
        if (!track->downstream_id)
        {
            block_ChainRelease(block);
            return VLC_SUCCESS;
        }
        return sout_StreamIdSend(stream->p_next, track->downstream_id, block);
    }

    if (sys->hls_failed)
    {
        block_ChainRelease(block);
        return VLC_SUCCESS;
    }

    sys->events.push_back({ false, track, block, 0 });
    return VLC_SUCCESS;
}

static void CacheFlush(sout_stream_t *stream, void *id)
{
    cc_startup_cache_sys_t *sys = CacheSys(stream);
    cc_cache_track_t *track = static_cast<cc_cache_track_t *>(id);

    if (sys->ready && track->downstream_id && stream->p_next)
        sout_StreamFlush(stream->p_next, track->downstream_id);
}

static int CacheControl(sout_stream_t *stream, int query, va_list args)
{
    if (stream->p_next)
        return sout_StreamControlVa(stream->p_next, query, args);
    return VLC_EGENERIC;
}

static void CacheSetPCR(sout_stream_t *stream, vlc_tick_t pcr)
{
    cc_startup_cache_sys_t *sys = CacheSys(stream);

    if (sys->ready)
    {
        sout_StreamSetPCR(stream->p_next, pcr);
        return;
    }

    if (sys->hls_failed)
        return;

    if (sys->first_pcr == VLC_TICK_INVALID)
    {
        msg_Dbg(stream, "startup caching started at PCR %" PRId64, pcr);
        sys->first_pcr = pcr;
    }

    sys->events.push_back({ true, nullptr, nullptr, pcr });

    vlc_tick_t elapsed = pcr - sys->first_pcr;
    if (elapsed >= CC_STARTUP_CACHING)
    {
        msg_Dbg(stream, "caching window elapsed (%" PRId64 " ms, %zu events) — building HLS chain",
                MS_FROM_VLC_TICK(elapsed), sys->events.size());
        if (!sys->build_cb(sys->build_opaque, stream, sys))
        {
            sys->hls_failed = true;
            msg_Err(stream, "HLS chain build failed");
        }
    }
}

static void CacheClose(sout_stream_t *stream)
{
    cc_startup_cache_sys_t *sys = CacheSys(stream);

    /* Remove track ids from the downstream chain before sout_StreamChainDelete
     * walks p_next and destroys it. */
    if (stream->p_next)
    {
        if (sys->hls_ready_fn)
            var_DelCallback(sys->hls_out, "hls-ready",
                            sys->hls_ready_fn, sys->hls_ready_data);
        for (auto *t : sys->tracks)
            if (t->downstream_id)
                sout_StreamIdDel(stream->p_next, t->downstream_id);
        /* Do NOT call sout_StreamChainDelete here: sout_StreamChainDelete
         * called on this stream will walk and free p_next itself. */
    }

    DropEvents(sys);

    for (auto *t : sys->tracks)
    {
        es_format_Clean(&t->fmt);
        free(t->es_id);
        delete t;
    }

    delete sys;
}

static const struct sout_stream_operations cache_ops =
{
    .add     = CacheAdd,
    .del     = CacheDel,
    .send    = CacheSend,
    .control = CacheControl,
    .flush   = CacheFlush,
    .set_pcr = CacheSetPCR,
    .close   = CacheClose,
};

/*****************************************************************************
 * Public API
 *****************************************************************************/

sout_stream_t *cc_startup_cache_New(sout_stream_t *parent,
                                    cc_cache_build_fn build_cb,
                                    void *opaque)
{
    sout_stream_t *cache =
        sout_StreamNew(VLC_OBJECT(parent), "cc-startup-cache{}");
    if (unlikely(!cache))
        return nullptr;

    cc_startup_cache_sys_t *sys = new (std::nothrow) cc_startup_cache_sys_t();
    if (unlikely(!sys))
    {
        sout_StreamChainDelete(cache, nullptr);
        return nullptr;
    }

    sys->build_cb       = build_cb;
    sys->build_opaque   = opaque;
    sys->first_pcr      = VLC_TICK_INVALID;
    sys->ready          = false;
    sys->hls_failed     = false;
    sys->hls_out        = nullptr;
    sys->hls_ready_fn   = nullptr;
    sys->hls_ready_data = nullptr;

    cache->ops   = &cache_ops;
    cache->p_sys = sys;

    return cache;
}

void cc_startup_cache_Reset(sout_stream_t *cache)
{
    cc_startup_cache_sys_t *sys = CacheSys(cache);

    msg_Dbg(cache, "cache reset (was %s, %zu buffered events)",
            sys->ready ? "ready" : "buffering", sys->events.size());

    TearDownChain(cache);
    DropEvents(sys);

    for (auto *t : sys->tracks)
    {
        t->downstream_id = nullptr;
        t->selected      = false;
    }

    sys->ready      = false;
    sys->hls_failed = false;
    sys->first_pcr  = VLC_TICK_INVALID;
}
