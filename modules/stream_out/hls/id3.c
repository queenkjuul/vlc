#include "common.h"

#include <vlc_block.h>

static uint32_t id3_SyncIntegerEncode( uint32_t in )
{
    uint32_t out, mask = 0x7F;

    while ( mask ^ 0x7FFFFFFF )
    {
        out = in & ~mask;
        out <<= 1;
        out |= in & mask;
        mask = ( ( mask + 1 ) << 8 ) - 1;
        in = out;
    }

    return out;
}

static void id3_DumpUintIntoBuffer( uint32_t in, uint8_t buffer[4] )
{
    buffer[0] = ( in & 0xff000000 ) >> 18;
    buffer[1] = ( in & 0xff0000 ) >> 12;
    buffer[2] = ( in & 0xff00 ) >> 8;
    buffer[3] = ( in & 0xff );
}

#define ID3_HDR_SIZE ( 10u )
#define ID3_FRAMEHDR_SIZE ( 10u )

static void fill_hdr( uint8_t hdr[ID3_HDR_SIZE], uint32_t id3_size )
{
    static const char identifier[3] = "ID3";
    memcpy( hdr, identifier, sizeof( identifier ) );

    static const uint8_t version[2] = { 2, 4 };
    memcpy( hdr + 3, version, sizeof( version ) );

    // Clear Flags
    hdr[5] = 0;

    id3_DumpUintIntoBuffer( id3_SyncIntegerEncode( id3_size ), hdr + 6 );
}

static void fill_tag( uint8_t *buffer, vlc_fourcc_t id, const uint8_t *tag, uint32_t tag_size )
{
    vlc_fourcc_to_char( id, (char *)buffer );
    buffer += sizeof( id );

    id3_DumpUintIntoBuffer( id3_SyncIntegerEncode( tag_size ), buffer );
    buffer += sizeof( tag_size );

    static const uint8_t flags[2] = { 0x10 /* only set read-only flag */, 0x0 };
    memcpy( buffer, &flags, sizeof( flags ) );
    buffer += sizeof( flags );

    memcpy( buffer, tag, tag_size );
}

block_t *Add_ID3( block_t *data )
{
#define ID3_HLS_TAG "com.apple.streaming.transportStreamTimestamp"
#define ID3_HLS_TAG_SIZE ( sizeof( ID3_HLS_TAG ) + sizeof( uint64_t ) )
    static const vlc_fourcc_t id = VLC_FOURCC( 'P', 'R', 'I', 'V' );

    static const size_t id3_total_size = ID3_HDR_SIZE + ID3_FRAMEHDR_SIZE + ID3_HLS_TAG_SIZE;
    block_t *ret = block_Realloc( data, id3_total_size, data->i_buffer );
    if ( unlikely( ret == NULL ) )
        return NULL;

    fill_hdr( ret->p_buffer, id3_total_size - ID3_HDR_SIZE );

    uint8_t tag[ID3_HLS_TAG_SIZE] = ID3_HLS_TAG;
    const uint64_t timestamp = NS_FROM_VLC_TICK(data->i_pts);
    SetQWBE( tag + sizeof( ID3_HLS_TAG ), id3_SyncIntegerEncode( timestamp ) );

    uint8_t *const id3_tag = ret->p_buffer + ID3_HDR_SIZE;
    fill_tag( id3_tag, id, tag, ID3_HLS_TAG_SIZE );

    return ret;
}
