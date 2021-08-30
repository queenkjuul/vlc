#include "cxx_test_helper.hpp"

#include "../webvtt.h"

extern "C"
{
#include <vlc_sout.h>
}

#include <iostream>
#include <sstream>
#include <string>

struct WebVTTInput
{
    vlc_tick_t start;
    vlc_tick_t stop;
    std::string text;

    block_t *to_block() const
    {
        block_t *ret = block_Alloc( text.size() + 1 );
        memcpy( ret->p_buffer, text.data(), text.size() );
        ret->p_buffer[text.size()] = '\0';

        ret->i_dts = start;
        ret->i_length = stop - start;
        return ret;
    }
};

class WebVttFixture : public test::Test
{
    sout_access_out_t access;

    protected:
    sout_mux_sys_t sys;
    std::stringstream out;
    block_fifo_t *fifo;

    void add_webvtt_subs( std::initializer_list<WebVTTInput> inputs )
    {
        for ( const auto &input : inputs )
        {
            block_FifoPut( fifo, input.to_block() );
        }
    }

    void add_header_flag(vlc_tick_t dts)
    {
        block_t *hdr = block_Alloc( 0 );
        hdr->i_flags |= BLOCK_FLAG_HEADER;
        hdr->i_dts = dts;
        block_FifoPut( fifo, hdr );
    }

    void manual_drain() { sys_FlushSegment( &sys ); }

    void call_mux() { sys_ProcessBlocks( &sys, fifo ); }

    public:
    void init() final
    {
        access.p_sys = &out;
        access.pf_write = []( sout_access_out_t *access, block_t *blocks ) -> ssize_t
        {
            auto *out = static_cast<std::stringstream *>( access->p_sys );
            while ( blocks )
            {
                out->write( (char *)blocks->p_buffer, blocks->i_buffer );

                block_t *next = blocks->p_next;
                block_Release( blocks );
                blocks = next;
            }
            return 0;
        };
        sys.access = &access;

        sys.segment.end = &sys.segment.begin;
        sys.access_write_fn = []( sout_mux_sys_t *sys, block_t *block ) -> ssize_t
        { return sys->access->pf_write( sys->access, block ); };
        fifo = block_FifoNew();
    }

    void release() final { block_FifoRelease( fifo ); }
};

TEST_F( WebVttFixture, SingleSegmentNotFlushed )
{
    add_webvtt_subs( { { VLC_TICK_FROM_MS( 200 ), VLC_TICK_FROM_MS( 500 ), "Test payload 1" },
                       { VLC_TICK_FROM_MS( 500 ), VLC_TICK_FROM_MS( 1000 ), "Test payload 2" } } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // This segment should stay in the muxer fifo as there is no other call to `mux` to flush it.
    ASSERT_EQ( out.str(), "" );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_SEC( 1 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.199 -> 0:00:00.499\n"
                                      "Test payload 1\n\n"
                                      "0:00:00.499 -> 0:00:00.999\n"
                                      "Test payload 2\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, SingleSegmentNotFlushedNotContinuous )
{
    add_webvtt_subs( { { VLC_TICK_FROM_MS( 200 ), VLC_TICK_FROM_MS( 500 ), "Test payload 1" },
                       { VLC_TICK_FROM_MS( 550 ), VLC_TICK_FROM_MS( 1000 ), "Test payload 2" } } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // This segment should stay in the muxer fifo as there is no other call to `mux` to flush it.
    ASSERT_EQ( out.str(), "" );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_SEC( 1 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.199 -> 0:00:00.499\n"
                                      "Test payload 1\n\n"
                                      "0:00:00.549 -> 0:00:00.999\n"
                                      "Test payload 2\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, TwoSegments )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 200 ), VLC_TICK_FROM_MS( 500 ), "Test payload 1" },
        { VLC_TICK_FROM_MS( 500 ), VLC_TICK_FROM_MS( 1000 ), "Test payload 2" },
        { VLC_TICK_FROM_MS( 1000 ), VLC_TICK_FROM_MS( 1200 ), "Test payload 3" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 1u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 200 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.199 -> 0:00:00.499\n"
                                      "Test payload 1\n\n"
                                      "0:00:00.499 -> 0:00:00.999\n"
                                      "Test payload 2\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:00.999 -> 0:00:01.199\n"
                                      "Test payload 3\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, TwoSegmentsTwo )
{
    add_webvtt_subs( {
        { VLC_TICK_0, VLC_TICK_FROM_MS( 100 ), "Test payload 1" },
        { VLC_TICK_FROM_MS( 1500 ), VLC_TICK_FROM_MS( 1633 ), "Test payload 2" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 1u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 633 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.000 -> 0:00:00.099\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:01.499 -> 0:00:01.632\n"
                                      "Test payload 2\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, TwoSegmentsThree )
{
    add_webvtt_subs( {
        { VLC_TICK_0, VLC_TICK_FROM_MS( 2 ), "Test payload 1" },
        { VLC_TICK_FROM_MS( 1999 ), VLC_TICK_FROM_MS( 2000 ), "Test payload 2" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 1u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 1000 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.000 -> 0:00:00.001\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:01.998 -> 0:00:01.999\n"
                                      "Test payload 2\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, TwoSegmentsNotContinuous )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 200 ), VLC_TICK_FROM_MS( 500 ), "Test payload 1" },
        { VLC_TICK_FROM_MS( 1200 ), VLC_TICK_FROM_MS( 1250 ), "Test payload 2" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 1u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 250 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.199 -> 0:00:00.499\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:01.199 -> 0:00:01.249\n"
                                      "Test payload 2\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, SimpleSegmentsThenOverlaping )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 200 ), VLC_TICK_FROM_MS( 500 ), "Test payload 1" },
        { VLC_TICK_FROM_MS( 500 ), VLC_TICK_FROM_MS( 1300 ), "Test payload 2" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 1u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 300 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.199 -> 0:00:00.499\n"
                                      "Test payload 1\n\n"
                                      "0:00:00.499 -> 0:00:00.999\n"
                                      "Test payload 2\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:00.999 -> 0:00:01.299\n"
                                      "Test payload 2\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, OverlapingThenSimpleSegment )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 180 ), VLC_TICK_FROM_MS( 1800 ), "Test payload 1" },
        { VLC_TICK_FROM_MS( 1800 ), VLC_TICK_FROM_MS( 2000 ), "Test payload 2" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 1u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_SEC( 1 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.179 -> 0:00:00.999\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:00.999 -> 0:00:01.799\n"
                                      "Test payload 1\n\n"
                                      "0:00:01.799 -> 0:00:01.999\n"
                                      "Test payload 2\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, FiveSegmentsOverlaping )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 822 ), VLC_TICK_FROM_MS( 5540 ), "Test payload 1" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 5u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 540 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.821 -> 0:00:00.999\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:00.999 -> 0:00:01.999\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:01.999 -> 0:00:02.999\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:02.999 -> 0:00:03.999\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:03.999 -> 0:00:04.999\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:04.999 -> 0:00:05.539\n"
                                      "Test payload 1\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, OneEmptySegment )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 200 ), VLC_TICK_FROM_MS( 500 ), "Test payload 1" },
        { VLC_TICK_FROM_MS( 2500 ), VLC_TICK_FROM_MS( 2600 ), "Test payload 2" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 2u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 600 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.199 -> 0:00:00.499\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:02.499 -> 0:00:02.599\n"
                                      "Test payload 2\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, OneEmptySegmentAtTheBeginning )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 1200 ), VLC_TICK_FROM_MS( 1500 ), "Test payload 1" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 1u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 500 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:01.199 -> 0:00:01.499\n"
                                      "Test payload 1\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, ThreeEmptySegment )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 200 ), VLC_TICK_FROM_MS( 500 ), "Test payload 1" },
        { VLC_TICK_FROM_MS( 2500 ), VLC_TICK_FROM_MS( 2600 ), "Test payload 2" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 2u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 600 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.199 -> 0:00:00.499\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:02.499 -> 0:00:02.599\n"
                                      "Test payload 2\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, PlentyOfSimpleSegments )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 200 ), VLC_TICK_FROM_MS( 500 ), "Test payload 1" },
        { VLC_TICK_FROM_MS( 1600 ), VLC_TICK_FROM_MS( 1900 ), "Test payload 2" },
        { VLC_TICK_FROM_MS( 2100 ), VLC_TICK_FROM_MS( 2400 ), "Test payload 3" },
        { VLC_TICK_FROM_MS( 3555 ), VLC_TICK_FROM_MS( 3666 ), "Test payload 4" },
        { VLC_TICK_FROM_MS( 4777 ), VLC_TICK_FROM_MS( 4888 ), "Test payload 5" },
        { VLC_TICK_FROM_MS( 5900 ), VLC_TICK_FROM_MS( 6000 ), "Test payload 6" },
        { VLC_TICK_FROM_MS( 6997 ), VLC_TICK_FROM_MS( 6998 ), "Test payload 7" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 6u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 998 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.199 -> 0:00:00.499\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:01.599 -> 0:00:01.899\n"
                                      "Test payload 2\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:02.099 -> 0:00:02.399\n"
                                      "Test payload 3\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:03.554 -> 0:00:03.665\n"
                                      "Test payload 4\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:04.776 -> 0:00:04.887\n"
                                      "Test payload 5\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:05.899 -> 0:00:05.999\n"
                                      "Test payload 6\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:06.996 -> 0:00:06.997\n"
                                      "Test payload 7\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, CutSegment )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 200 ), VLC_TICK_FROM_MS( 500 ), "Test payload 1" },
    } );
    add_header_flag( VLC_TICK_FROM_MS( 600 ) );
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 700 ), VLC_TICK_FROM_MS( 800 ), "Test payload 2" },
    } );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 1u );
    //ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 998 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.199 -> 0:00:00.499\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:00.699 -> 0:00:00.799\n"
                                      "Test payload 2\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, CutSegmentOverlap )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 200 ), VLC_TICK_FROM_MS( 800 ), "Test payload 1" },
    } );
    add_header_flag( VLC_TICK_FROM_MS( 450 ) );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 1u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 350 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.199 -> 0:00:00.449\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:00.449 -> 0:00:00.799\n"
                                      "Test payload 1\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

TEST_F( WebVttFixture, CutSegmentOverlap2 )
{
    add_webvtt_subs( {
        { VLC_TICK_FROM_MS( 200 ), VLC_TICK_FROM_MS( 800 ), "Test payload 1" },
    } );
    add_header_flag( VLC_TICK_FROM_MS( 450 ) );
    add_header_flag( VLC_TICK_FROM_MS( 650 ) );

    sys.segment_max_length = VLC_TICK_FROM_SEC( 1 );
    call_mux();

    // First segment should have already been flushed once.
    ASSERT_FALSE( out.str().empty() );
    ASSERT_EQ( sys.segment_count, 2u );
    ASSERT_EQ( sys.segment.length, VLC_TICK_FROM_MS( 150 ) );

    manual_drain();

    // clang-format off
    static constexpr char segment[] = "WEBVTT\n\n"
                                      "0:00:00.199 -> 0:00:00.449\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:00.449 -> 0:00:00.649\n"
                                      "Test payload 1\n\n"
                                      "WEBVTT\n\n"
                                      "0:00:00.649 -> 0:00:00.799\n"
                                      "Test payload 1\n\n";
    // clang-format on

    ASSERT_EQ( out.str(), segment );
}

int main() { return test::TestPool::run_all( test::Logger() ) ? EXIT_SUCCESS : EXIT_FAILURE; }
