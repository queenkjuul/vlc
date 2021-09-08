/*****************************************************************************
 * sout.cpp
 *****************************************************************************
 * Copyright © 2019 VLC authors and VideoLAN
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
#include "upnp_server.hpp"

#include <vlc_sout.h>
#include <vlc_stream.h>
#include <vlc_stream_extractor.h>

const char TranscodeFifo::VAR_NAME[] = SERVER_PREFIX "fifo";

size_t TranscodeFifo::read(uint8_t buf[], size_t buflen) noexcept
{
    vlc_fifo_Lock(queue.get());

    size_t bytes;
    while ((bytes = vlc_fifo_GetBytes(queue.get())) < buflen / 8 && !eof)
    {
        vlc_fifo_Wait(queue.get());
    }
    buflen = std::min(buflen, bytes);

    size_t bytes_read = 0;
    while (bytes_read < buflen)
    {
        block_t *block = vlc_fifo_DequeueUnlocked(queue.get());
        assert(block);

        const size_t to_copy = std::min(block->i_buffer, buflen - bytes_read);
        memcpy(buf + bytes_read, block->p_buffer, to_copy);
        bytes_read += to_copy;

        block->p_buffer += to_copy;
        block->i_buffer -= to_copy;

        if (block->i_buffer == 0)
            block_Release(block);
        else
        {
            block_t *all = vlc_fifo_DequeueAllUnlocked(queue.get());
            block->p_next = all;
            vlc_fifo_QueueUnlocked(queue.get(), block);
            assert(bytes_read == buflen);
        }
    }

    vlc_fifo_Signal(queue.get());
    vlc_fifo_Unlock(queue.get());

    return bytes_read;
}

namespace Server
{
namespace AccessOut
{
int open(vlc_object_t *p_this)
{
    sout_access_out_t *p_access = reinterpret_cast<sout_access_out_t *>(p_this);

    p_access->p_sys = var_InheritAddress(p_this, TranscodeFifo::VAR_NAME);

    p_access->pf_write = [](sout_access_out_t *access, block_t *data) -> ssize_t {
        auto *fifo = static_cast<TranscodeFifo *>(access->p_sys);

        size_t size = 0;
        block_ChainProperties(data, nullptr, &size, nullptr);

        vlc_fifo_Lock(fifo->queue.get());

        // The fifo holds too much data, wait for the http thread to empty it.
        static constexpr auto FIFO_MAX_SIZE = INT64_C(2 * 1024 * 1024); /* 2 MB */
        while (vlc_fifo_GetBytes(fifo->queue.get()) >= FIFO_MAX_SIZE)
        {
            msg_Warn(access, "Fifo buffer full, pacing the transcode es out.");
            vlc_fifo_Wait(fifo->queue.get());
        }

        vlc_fifo_QueueUnlocked(fifo->queue.get(), data);
        vlc_fifo_Unlock(fifo->queue.get());

        // No needs to signal the fifo cond_var as vlc_fifo_QueueUnlocked
        // already does it

        return size;
    };

    p_access->pf_control = [](sout_access_out_t *, int i_query, va_list args) -> int {
        switch (i_query)
        {
            case ACCESS_OUT_CONTROLS_PACE:
                *va_arg(args, bool *) = true;
                return VLC_SUCCESS;
            case ACCESS_OUT_CAN_SEEK:
                *va_arg(args, bool *) = false;
                return VLC_SUCCESS;
        }
        return VLC_ENOTSUP;
    };

    p_access->pf_seek = nullptr;
    p_access->pf_read = nullptr;

    return VLC_SUCCESS;
}

void close(vlc_object_t *p_this)
{
    auto *fifo = static_cast<TranscodeFifo *>(var_InheritAddress(p_this, TranscodeFifo::VAR_NAME));
    assert(fifo);

    vlc_fifo_t *queue = fifo->queue.get();
    vlc_fifo_Lock(queue);
    fifo->eof = true;
    vlc_fifo_Unlock(queue);

    vlc_fifo_Signal(queue);
}
} // namespace AccessOut

namespace StreamOutProxy
{

struct sout_stream_sys_t
{
    bool is_flushed = false;
};

static void *add(sout_stream_t *stream, const es_format_t *fmt, const char *es_id)
{
    return sout_StreamIdAdd(stream->p_next, fmt, es_id);
}

static void del(sout_stream_t *stream, void *id) { return sout_StreamIdDel(stream->p_next, id); }

static int send(sout_stream_t *stream, void *id, vlc_frame_t *frame)
{
    const sout_stream_sys_t *sys = static_cast<sout_stream_sys_t *>(stream->p_sys);
    if (!sys->is_flushed)
    {
        vlc_frame_Release(frame);
        return VLC_SUCCESS;
    }
    return sout_StreamIdSend(stream->p_next, id, frame);
}

static void flush(sout_stream_t *stream, void *id)
{
    sout_stream_sys_t *sys = static_cast<sout_stream_sys_t *>(stream->p_sys);
    sys->is_flushed = true;
    sout_StreamFlush(stream->p_next, id);
}

static void set_pcr(sout_stream_t *stream, vlc_tick_t pcr)
{
    const sout_stream_sys_t *sys = static_cast<sout_stream_sys_t *>(stream->p_sys);
    if (!sys->is_flushed)
        return;

    sout_StreamSetPCR(stream->p_next, pcr);
}

static void close(sout_stream_t *stream) { delete static_cast<sout_stream_sys_t *>(stream->p_sys); }

static const sout_stream_operations operations = [] {
    sout_stream_operations ops;
    ops.add = add;
    ops.del = del;
    ops.send = send;
    ops.flush = flush;
    ops.set_pcr = set_pcr;
    ops.close = close;
    return ops;
}();

int open(vlc_object_t *p_this)
{
    sout_stream_t *p_stream = reinterpret_cast<sout_stream_t *>(p_this);

    auto *sys = new (std::nothrow) sout_stream_sys_t;
    if (unlikely(sys == nullptr))
        return VLC_ENOMEM;

    p_stream->p_sys = sys;
    p_stream->ops = &operations;
    return VLC_SUCCESS;
}

} // namespace StreamOutProxy
} // namespace Server
