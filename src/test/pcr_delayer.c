/*****************************************************************************
 * src/test/pcr_delayer.c
 *****************************************************************************
 * Copyright (C) 2022 Videolabs, VLC authors and VideoLAN
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#undef NDEBUG

#include <assert.h>

#include <vlc_common.h>

#include <vlc_block.h>
#include <vlc_pcr_delayer.h>
#include <vlc_tick.h>
#include <vlc_vector.h>

struct block_ts
{
    vlc_tick_t dts;
    vlc_tick_t len;
};

struct test_element
{
    enum test_element_type
    {
        TEST_ELEM_TYPE_BLOCK,
        TEST_ELEM_TYPE_PCR,
    } type;
    union
    {
        struct block_ts block;
        vlc_tick_t pcr;
    };
};

static inline struct test_element test_MakeBlock( vlc_tick_t dts, vlc_tick_t len )
{
    return ( struct test_element ){
        .type = TEST_ELEM_TYPE_BLOCK, .block.dts = dts, .block.len = len };
}
#define TBLOCK( d, l ) test_MakeBlock( d, l )

static inline struct test_element test_MakePCR( vlc_tick_t pcr )
{
    return ( struct test_element ){ .type = TEST_ELEM_TYPE_PCR, .pcr = pcr };
}
#define TPCR( p ) test_MakePCR( p )

typedef struct VLC_VECTOR( struct test_element ) elem_vector_t;

typedef struct
{
    elem_vector_t result;
} delayer_send_mock_sys_t;

static void SetPCRMock( void *opaque, vlc_tick_t pcr )
{
    delayer_send_mock_sys_t *sys = opaque;

    vlc_vector_push( &sys->result, test_MakePCR( pcr ) );
}

static int SendBlocksMock( void *opaque, block_t *blocks )
{
    delayer_send_mock_sys_t *sys = opaque;
    for ( const block_t *it = blocks; it != NULL; it = it->p_next )
    {
        vlc_vector_push( &sys->result, test_MakeBlock( it->i_dts, it->i_length ) );
    }
    return VLC_SUCCESS;
}

static void
test_PCROnly( vlc_pcr_delayer_t *delayer, delayer_send_mock_sys_t *sys, const vlc_tick_t *pcrs )
{
    unsigned count = 0;
    for ( ; pcrs[count] != VLC_TICK_INVALID; ++count )
    {
        const vlc_pcr_delayer_status_t status =
            vlc_pcr_delayer_SignalPCR( delayer, pcrs[count], VLC_TICK_INVALID );
        assert( status == VLC_PCR_DELAYER_NO_DELAY );
    }

    vlc_pcr_delayer_SendBlockedPCR( delayer, sys, SetPCRMock );
    assert( sys->result.size == count );
    for ( unsigned i = 0; i < count; ++i )
    {
        assert( sys->result.data[i].type == TEST_ELEM_TYPE_PCR );
        assert( sys->result.data[i].pcr == pcrs[i] );
    }
}

static void test_OnePCRZero( vlc_pcr_delayer_t *delayer, delayer_send_mock_sys_t *sys )
{
    test_PCROnly( delayer, sys, ( vlc_tick_t[] ){ 1, VLC_TICK_INVALID } );
}
static void test_OnePCRTwenty( vlc_pcr_delayer_t *delayer, delayer_send_mock_sys_t *sys )
{
    test_PCROnly( delayer, sys, ( vlc_tick_t[] ){ 20, VLC_TICK_INVALID } );
}
static void test_MultiplePCR1( vlc_pcr_delayer_t *delayer, delayer_send_mock_sys_t *sys )
{
    test_PCROnly( delayer, sys, ( vlc_tick_t[] ){ 1, 2, 3, 4, 5, 6, VLC_TICK_INVALID } );
}
static void test_MultiplePCR2( vlc_pcr_delayer_t *delayer, delayer_send_mock_sys_t *sys )
{
    test_PCROnly( delayer, sys, ( vlc_tick_t[] ){ 4342, 7439, 88888, VLC_TICK_INVALID } );
}

static void test_NoFirstPCR( vlc_pcr_delayer_t *delayer, delayer_send_mock_sys_t *sys )
{
    block_t dummy = { .i_dts = 1, .i_length = 20 };
    block_t *b_ref = &dummy;
    vlc_pcr_delayer_SendBlocksAndPCR( delayer, &b_ref, sys, SendBlocksMock, SetPCRMock );

    assert( sys->result.size == 1 );
    const struct test_element *e = &sys->result.data[0];
    assert( e->type == TEST_ELEM_TYPE_BLOCK );
    assert( e->block.dts == 1 );
    assert( e->block.len == 20 );
}
typedef struct
{
    const struct test_element *array;
    size_t count;
} test_elements_t;

typedef struct
{
    const struct block_ts *array;
    size_t count;
} test_blocks_t;
#define sizeof_array( array ) ( sizeof( array ) / sizeof( *array ) )

//           +--> Enter -> delay -> Leave +-> Send
//           |                            |
// BLOCKS ---+                            |
//                                        |
// PCR ------> SetPCR --------------------+
//
static void test_PCRAndBlocks( vlc_pcr_delayer_t *delayer,
                               delayer_send_mock_sys_t *sys,
                               test_elements_t in,
                               const test_blocks_t *block_out,
                               test_elements_t expected )
{
    vlc_tick_t last_dts = VLC_TICK_INVALID;
    for ( size_t i = 0; i < in.count; ++i )
    {
        const struct test_element *e = &in.array[i];
        if ( e->type == TEST_ELEM_TYPE_BLOCK )
        {
            last_dts = e->block.dts;
            block_t b = { .i_dts = e->block.dts, .i_length = e->block.len };
            vlc_pcr_delayer_SignalEnteringBlock( delayer, &b );

            if ( block_out == NULL )
            {
                vlc_pcr_delayer_SignalLeavingBlock( delayer, &b );
                block_t *b_ref = &b;
                vlc_pcr_delayer_SendBlocksAndPCR( delayer, &b_ref, sys, SendBlocksMock,
                                                  SetPCRMock );
            }
        }
        else
            vlc_pcr_delayer_SignalPCR( delayer, e->pcr, last_dts );
    }

    if ( block_out != NULL )
    {
        for ( size_t i = 0; i < block_out->count; ++i )
        {
            const struct block_ts *b = &block_out->array[i];
            block_t dummy = { .i_dts = b->dts, .i_length = b->len };
            block_t *ref = &dummy;
            vlc_pcr_delayer_SignalLeavingBlock( delayer, ref );

            vlc_pcr_delayer_SendBlocksAndPCR( delayer, &ref, sys, SendBlocksMock, SetPCRMock );
        }
    }

    vlc_pcr_delayer_Drain( delayer, sys, SetPCRMock );

    assert( expected.count == sys->result.size );
    for ( size_t i = 0; i < expected.count; ++i )
    {
        const struct test_element *e = &expected.array[i];
        const struct test_element *result = &sys->result.data[i];
        assert( e->type == result->type );
        if ( e->type == TEST_ELEM_TYPE_BLOCK )
        {
            assert( result->block.dts == e->block.dts );
            assert( result->block.len == e->block.len );
        }
        else
        {
            assert( result->pcr == e->pcr );
        }
    }
}

static void test_PCRAndBlocksSimple( vlc_pcr_delayer_t *delayer, delayer_send_mock_sys_t *sys )
{
    const struct test_element in[] = { TPCR( 1 ), TPCR( 25 ), TBLOCK( 1, 25 ) };
    test_PCRAndBlocks( delayer, sys,
                       ( test_elements_t ){ .array = in, .count = sizeof_array( in ) }, NULL,
                       ( test_elements_t ){ .array = in, .count = sizeof_array( in ) } );
}
static void test_PCRAndBlocksEmptyPCRs( vlc_pcr_delayer_t *delayer, delayer_send_mock_sys_t *sys )
{
    const struct test_element in[] = { TPCR( 1 ), TPCR( 25 ), TPCR( 65 ), TBLOCK( 75, 25 ) };
    const struct test_element expected[] = { TPCR( 1 ), TPCR( 25 ), TPCR( 65 ), TBLOCK( 75, 25 ) };
    test_PCRAndBlocks(
        delayer, sys, ( test_elements_t ){ .array = in, .count = sizeof_array( in ) }, NULL,
        ( test_elements_t ){ .array = expected, .count = sizeof_array( expected ) } );
}
static void test_PCRAndBlocksEmptyPCRsNoTrailingBlock( vlc_pcr_delayer_t *delayer,
                                                       delayer_send_mock_sys_t *sys )
{
    const struct test_element in[] = { TPCR( 1 ), TBLOCK( 2, 15 ), TPCR( 25 ), TPCR( 65 ) };
    test_PCRAndBlocks( delayer, sys,
                       ( test_elements_t ){ .array = in, .count = sizeof_array( in ) }, NULL,
                       ( test_elements_t ){ .array = in, .count = sizeof_array( in ) } );
}
static void test_NoPCROnlyBlocks( vlc_pcr_delayer_t *delayer, delayer_send_mock_sys_t *sys )
{
    const struct test_element in[] = { TBLOCK( 1, 10 ), TBLOCK( 11, 10 ), TBLOCK( 22, 10 ) };
    test_PCRAndBlocks( delayer, sys,
                       ( test_elements_t ){ .array = in, .count = sizeof_array( in ) }, NULL,
                       ( test_elements_t ){ .array = in, .count = sizeof_array( in ) } );
}
static void test_DelaySimplePositiveShift( vlc_pcr_delayer_t *delayer,
                                           delayer_send_mock_sys_t *sys )
{
    const struct test_element in[] = { TBLOCK( 1, 10 ), TBLOCK( 11, 10 ), TPCR( 22 ) };
    const struct block_ts out_blocks[] = {
        { .dts = 200, .len = 10 }, { .dts = 210, .len = 10 } };
    const struct test_element expected[] = { TBLOCK( 200, 10 ), TBLOCK( 210, 10 ), TPCR( 222 ) };
    test_PCRAndBlocks(
        delayer, sys, ( test_elements_t ){ .array = in, .count = sizeof_array( in ) },
        &( test_blocks_t ){ .array = out_blocks, .count = sizeof_array( out_blocks ) },
        ( test_elements_t ){ .array = expected, .count = sizeof_array( expected ) } );
}
static void test_DelaySimpleNegativeShift( vlc_pcr_delayer_t *delayer,
                                           delayer_send_mock_sys_t *sys )
{
    const struct test_element in[] = { TBLOCK( 200, 10 ), TBLOCK( 211, 10 ), TPCR( 222 ) };
    const struct block_ts out_blocks[] = { { .dts = 1, .len = 10 }, { .dts = 11, .len = 10 } };
    const struct test_element expected[] = { TBLOCK( 1, 10 ), TBLOCK( 11, 10 ), TPCR( 22 ) };
    test_PCRAndBlocks(
        delayer, sys, ( test_elements_t ){ .array = in, .count = sizeof_array( in ) },
        &( test_blocks_t ){ .array = out_blocks, .count = sizeof_array( out_blocks ) },
        ( test_elements_t ){ .array = expected, .count = sizeof_array( expected ) } );
}
static void test_DelayShiftTwoPCR( vlc_pcr_delayer_t *delayer,
                                           delayer_send_mock_sys_t *sys )
{
    const struct test_element in[] = { TBLOCK( 1, 10 ), TBLOCK( 11, 10 ), TPCR( 22 ), TBLOCK(22, 10), TPCR(33) };
    const struct block_ts out_blocks[] = {
        { .dts = 200, .len = 10 }, { .dts = 210, .len = 10 }, { .dts = 220, .len = 10 } };
    const struct test_element expected[] = { TBLOCK( 200, 10 ), TBLOCK( 210, 10 ), TPCR( 222 ), TBLOCK(220, 10), TPCR(33) };
    test_PCRAndBlocks(
        delayer, sys, ( test_elements_t ){ .array = in, .count = sizeof_array( in ) },
        &( test_blocks_t ){ .array = out_blocks, .count = sizeof_array( out_blocks ) },
        ( test_elements_t ){ .array = expected, .count = sizeof_array( expected ) } );
}
static void test_Delay( vlc_pcr_delayer_t *delayer,
                                                delayer_send_mock_sys_t *sys )
{
    const struct test_element in[] = { TPCR( 1 ), TBLOCK( 1, 10 ), TBLOCK( 11, 10 ), TPCR( 200 ) };
    const struct block_ts out_blocks[] = { { .dts = 200, .len = 10 }, { .dts = 210, .len = 10 } };
    const struct test_element expected[] = { TPCR( 1 ), TBLOCK( 200, 10 ), TBLOCK( 210, 10 ),
                                             TPCR( 210 ) };
    test_PCRAndBlocks(
        delayer, sys, ( test_elements_t ){ .array = in, .count = sizeof_array( in ) },
        &( test_blocks_t ){ .array = out_blocks, .count = sizeof_array( out_blocks ) },
        ( test_elements_t ){ .array = expected, .count = sizeof_array( expected ) } );
}

static void test_run( vlc_tick_t delay,
                      void ( *test )( vlc_pcr_delayer_t *, delayer_send_mock_sys_t * ) )
{
    vlc_pcr_delayer_t *delayer = vlc_pcr_delayer_New( delay );
    assert( delayer != NULL );

    delayer_send_mock_sys_t sys = { .result = VLC_VECTOR_INITIALIZER };

    test( delayer, &sys );

    vlc_vector_destroy( &sys.result );

    vlc_pcr_delayer_Delete( delayer );
}

int main( void )
{
    static const vlc_tick_t default_delay = VLC_TICK_FROM_MS( 200 );
///    test_run( default_delay, test_OnePCRZero );
///    test_run( default_delay, test_OnePCRTwenty );
///    test_run( default_delay, test_MultiplePCR1 );
///    test_run( default_delay, test_MultiplePCR2 );
///    test_run( default_delay, test_NoFirstPCR );
///
///    test_run( default_delay, test_PCRAndBlocksSimple );
///    test_run( default_delay, test_PCRAndBlocksEmptyPCRs );
///    // TODO test_run( default_delay, test_PCRAndBlocksEmptyPCRsNoTrailingBlock );
///    test_run( default_delay, test_NoPCROnlyBlocks );
///
    test_run( default_delay, test_DelaySimplePositiveShift );
    //test_run( default_delay, test_DelaySimpleNegativeShift );
    // test_run( default_delay, test_PCRAndBlocksSimpleDifferentTS );

    return 0;
}
