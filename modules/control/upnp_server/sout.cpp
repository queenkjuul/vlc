#include "upnp_server.h"

#include <vlc_sout.h>
#include <vlc_stream.h>
#include <vlc_stream_extractor.h>

#include <iostream>
#include <atomic>

const char Fifo::VAR_NAME[] = "upnp-fifo";


size_t Fifo::read( uint8_t buf[], size_t buflen ) noexcept
{
   //static constexpr auto MIN_FIFO_SIZE = 1048576u / 4u;
    size_t bytes_read = 0;

    vlc_fifo_Lock( queue.get() );

    size_t bytes = 0;
    while ( !eof && ( bytes = vlc_fifo_GetBytes( queue.get() ) ) == 0 )
    {
        vlc_fifo_Wait( queue.get() );
    }
    buflen = MIN(buflen, bytes);

   // if ( eof )
   // {
   //     buflen = vlc_fifo_GetBytes( queue );
   // }

    while ( bytes_read < buflen )
    {
        block_t* block = vlc_fifo_DequeueUnlocked( queue.get() );
        assert(block);

        const size_t to_copy = MIN( block->i_buffer, buflen - bytes_read );
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

#define HTTPD_BUFFER_PACE INT64_C(2 * 1024 * 1024) /* 2 MB */
namespace Server
{
namespace AccessOut
{
int open( vlc_object_t* p_this )
{
    sout_access_out_t* p_access = reinterpret_cast<sout_access_out_t*>( p_this );

    p_access->p_sys = var_InheritAddress( p_this, Fifo::VAR_NAME );

    p_access->pf_write = []( sout_access_out_t* access, block_t* data ) -> ssize_t {
        if ( access->p_sys == nullptr )
        {
            return 0;
        }
        auto fifo = *static_cast<std::shared_ptr<Fifo>*>( access->p_sys );

        // TODO: set a maximum size for the fifo

        size_t size = 0;
        block_ChainProperties( data, nullptr, &size, nullptr );

        vlc_fifo_Lock( fifo->queue.get() );
        //while(vlc_fifo_GetBytes(fifo->queue) >= HTTPD_BUFFER_PACE) {
        //  std::cerr << "Waiting\n";
        //  vlc_fifo_Wait(fifo->queue);
        //}
        vlc_fifo_QueueUnlocked( fifo->queue.get(), data );
        vlc_fifo_Unlock( fifo->queue.get() );

        // No needs to signal the fifo cond_var as vlc_fifo_QueueUnlocked
        // already does it

        return size;
    };

    p_access->pf_control = []( sout_access_out_t* aout, int i_query, va_list args ) -> int {
        msg_Dbg( aout, "fifo control\n" );
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
        static_cast<std::shared_ptr<Fifo>*>( var_InheritAddress( p_this, Fifo::VAR_NAME ) );
    assert( fifo );

    auto* queue = ( *fifo )->queue.get();
    vlc_fifo_Lock( queue );
    ( *fifo )->eof = true;
    vlc_fifo_Unlock( queue );

    vlc_fifo_Signal( queue );

    reinterpret_cast<sout_access_out_t*>( p_this )->p_sys = nullptr;

    msg_Dbg( p_this, "fifo closed" );
}
} // namespace AccessOut

// This small sout is the entry point of the transcode chain when the client
// request the media with a specific starting time (other than 0).
// This simple stream makes sure that nothing is sent before the
// flush operation right after the player seek.
// This is a hacky way to ensure the client receive no discontinuity caused
// by seeking at the beginning of the stream.

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
