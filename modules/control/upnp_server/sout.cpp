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

#include <iostream>
#include <atomic>

const char TranscodeFifo::VAR_NAME[] = SERVER_PREFIX "fifo";


size_t TranscodeFifo::read( uint8_t buf[], size_t buflen ) noexcept
{
    vlc_fifo_Lock( queue.get() );

    size_t bytes;
    while ( ( bytes = vlc_fifo_GetBytes( queue.get() ) ) < buflen / 2 && !eof )
    {
        vlc_fifo_Wait( queue.get() );
    }
    buflen = std::min(buflen, bytes);

    size_t bytes_read = 0;
    while ( bytes_read < buflen )
    {
        block_t* block = vlc_fifo_DequeueUnlocked( queue.get() );
        assert(block);

        const size_t to_copy = std::min( block->i_buffer, buflen - bytes_read );
        memcpy( buf + bytes_read, block->p_buffer, to_copy );
        bytes_read += to_copy;

        block->p_buffer += to_copy;
        block->i_buffer -= to_copy;

        if ( block->i_buffer == 0 )
            block_Release( block );
        else
        {
            block_t* all = vlc_fifo_DequeueAllUnlocked( queue.get() );
            block->p_next = all;
            vlc_fifo_QueueUnlocked( queue.get(), block );
            assert(bytes_read == buflen);
        }
    }

    vlc_fifo_Signal( queue.get() );
    vlc_fifo_Unlock( queue.get() );

    return bytes_read;
}

namespace Server
{
namespace AccessOut
{
int open( vlc_object_t* p_this )
{
    sout_access_out_t* p_access = reinterpret_cast<sout_access_out_t*>( p_this );

    p_access->p_sys = var_InheritAddress( p_this, TranscodeFifo::VAR_NAME );

    p_access->pf_write = []( sout_access_out_t* access, block_t* data ) -> ssize_t {
        auto* fifo = static_cast<TranscodeFifo*>( access->p_sys );

        size_t size = 0;
        block_ChainProperties( data, nullptr, &size, nullptr );

        vlc_fifo_Lock( fifo->queue.get() );

        // The fifo holds too much data, wait for the http thread to empty it.
        static constexpr auto FIFO_MAX_SIZE = INT64_C(2 * 1024 * 1024); /* 2 MB */
        while ( vlc_fifo_GetBytes( fifo->queue.get() ) >= FIFO_MAX_SIZE )
        {
            msg_Warn( access, "Fifo buffer full, pacing the transcode es out." );
            vlc_fifo_Wait( fifo->queue.get() );
        }

        vlc_fifo_QueueUnlocked( fifo->queue.get(), data );
        vlc_fifo_Unlock( fifo->queue.get() );

        // No needs to signal the fifo cond_var as vlc_fifo_QueueUnlocked
        // already does it

        return size;
    };

    p_access->pf_control = []( sout_access_out_t*, int i_query, va_list args ) -> int {
        switch ( i_query )
        {
        case ACCESS_OUT_CONTROLS_PACE:
        {
            bool* pb = va_arg( args, bool* );
            *pb = false;
            break;
        }

        case ACCESS_OUT_CAN_SEEK:
        {
            bool* pb = va_arg( args, bool* );
            *pb = false;
            break;
        }

        default:
            return VLC_EGENERIC;
        }
        return VLC_SUCCESS;
    };

    p_access->pf_seek = nullptr;
    p_access->pf_read = nullptr;

    return VLC_SUCCESS;
}

void close( vlc_object_t* p_this )
{
    msg_Dbg( p_this, "Closing fifo" );

    auto* fifo =
        static_cast<TranscodeFifo*>( var_InheritAddress( p_this, TranscodeFifo::VAR_NAME ) );
    assert( fifo );

    vlc_fifo_t* queue = fifo->queue.get();
    vlc_fifo_Lock( queue );
    fifo->eof = true;
    vlc_fifo_Unlock( queue );

    vlc_fifo_Signal( queue );

    msg_Dbg( p_this, "Fifo closed" );
}
} // namespace AccessOut

namespace StreamOutProxy
{

struct sout_stream_sys_t
{
  std::atomic_bool is_flushed = ATOMIC_VAR_INIT(false);
};

static void* add( sout_stream_t* s, const es_format_t* fmt )
{
    return sout_StreamIdAdd( s->p_next, fmt );
}

static void del( sout_stream_t* s, void* id )
{
    return sout_StreamIdDel( s->p_next, id );
}

static int send( sout_stream_t* p_stream, void* id, block_t* p_buffer )
{
    const sout_stream_sys_t* p_sys = reinterpret_cast<sout_stream_sys_t*>( p_stream->p_sys );
    if ( !p_sys->is_flushed )
    {
        block_ChainRelease( p_buffer );
        return VLC_SUCCESS;
    }
    return sout_StreamIdSend( p_stream->p_next, id, p_buffer );
}


static void flush(sout_stream_t *p_stream, void* id)
{
  sout_stream_sys_t* p_sys = reinterpret_cast<sout_stream_sys_t*>( p_stream->p_sys );
  p_sys->is_flushed.store(true);
  sout_StreamFlush(p_stream->p_next, id);
}

static const sout_stream_operations operations = { add, del, send, nullptr, flush };

int open( vlc_object_t* p_this )
{
    sout_stream_t* p_stream = reinterpret_cast<sout_stream_t*>( p_this );

    auto* sys = new sout_stream_sys_t;

    p_stream->p_sys = sys;
    p_stream->ops = &operations;
    return VLC_SUCCESS;
}

void close( vlc_object_t* p_this )
{
    sout_stream_t* p_stream = reinterpret_cast<sout_stream_t*>( p_this );

    delete static_cast<sout_stream_sys_t*>( p_stream->p_sys );
}
} // namespace StreamOutProxy
} // namespace Server
