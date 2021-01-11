#include "upnp_server.h"

#include <vlc_sout.h>
#include <vlc_stream.h>
#include <vlc_stream_extractor.h>

const char Fifo::VAR_NAME[] = "upnp-fifo";

size_t Fifo::read( uint8_t buf[], size_t buflen ) noexcept
{
    size_t bytes_read = 0;
    size_t min_buff = buflen;

    vlc_fifo_Lock( queue );

    while ( !eof && vlc_fifo_GetBytes( queue ) < min_buff )
    {
        vlc_fifo_Wait( queue );
    }

    if ( eof )
    {
        min_buff = vlc_fifo_GetBytes( queue );
    }

    while ( bytes_read < min_buff )
    {
        block_t* block = vlc_fifo_DequeueUnlocked( queue );
        // TODO maybe send whats possible to send and shrink the block
        // before requeuing it
        if ( bytes_read + block->i_buffer >= min_buff )
        {
            vlc_fifo_QueueUnlocked( queue, block );
            break;
        }
        memcpy( buf + bytes_read, block->p_buffer, block->i_buffer );
        bytes_read += block->i_buffer;
        block_Release( block );
    }

    vlc_fifo_Unlock( queue );

    return bytes_read;
}

int Server::AccessOpen( vlc_object_t* p_this )
{
    sout_access_out_t* p_access = reinterpret_cast<sout_access_out_t*>( p_this );

    p_access->p_sys = var_InheritAddress( p_this, Fifo::VAR_NAME );

    p_access->pf_write = []( sout_access_out_t* access, block_t* data ) -> ssize_t {
        Fifo* fifo = static_cast<Fifo*>( access->p_sys );

        // TODO: set a maximum size for the fifo

        size_t size = 0;
        block_ChainProperties( data, nullptr, &size, nullptr );

        vlc_fifo_Lock( fifo->queue );
        vlc_fifo_QueueUnlocked( fifo->queue, data );
        vlc_fifo_Unlock( fifo->queue );

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
            *pb = true;
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

void Server::AccessClose( vlc_object_t* p_this )
{
    msg_Dbg( p_this, "Closing fifo\n" );

    Fifo* fifo = static_cast<Fifo*>( var_InheritAddress( p_this, Fifo::VAR_NAME ) );
    assert( fifo );

    auto* queue = fifo->queue;
    vlc_fifo_Lock( queue );
    fifo->eof = true;
    vlc_fifo_Unlock( queue );

    vlc_fifo_Signal( queue );

    msg_Dbg( p_this, "fifo closed\n" );
}
