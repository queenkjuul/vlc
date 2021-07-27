#undef NDEBUG
#include <assert.h>

#include "../common.h"

#include "meta_engine/ID3Meta.h"
#include "meta_engine/ID3Tag.h"

#include <vlc_block.h>

static int ID3TAG_Parse_Handler( uint32_t i_tag, const uint8_t *p_payload, size_t i_payload, void *priv )
{
    if ( i_tag != VLC_FOURCC( 'P', 'R', 'I', 'V' ) )
    {
        return VLC_EGENERIC;
    }

    const uint64_t encoded_timestamp = GetQWBE( p_payload + 45 );
    if ( i_payload == 53 &&
         !memcmp( p_payload, "com.apple.streaming.transportStreamTimestamp", 45 ) &&
         encoded_timestamp == 383 )
    {
        return VLC_SUCCESS;
    }
    assert(false);
    return VLC_EGENERIC;

    (void)priv;
}

int main()
{
    block_t *block = block_Alloc( 0x10 );
    memset( block->p_buffer, 0x42, block->i_buffer );

    block = Add_ID3( block , 0xff);

    const size_t res = ID3TAG_Parse( block->p_buffer, block->i_buffer, ID3TAG_Parse_Handler, NULL );
    assert(res == block->i_buffer - 0x10);
    for (int i = 0; i < 0x10; ++i) {
        assert(block->p_buffer[res + i] == 0x42);
    }
}
