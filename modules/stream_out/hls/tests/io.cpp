#include "cxx_test_helper.hpp"

#include "../common.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <vlc_common.h>

#include <vlc_block.h>

#include <memory>

struct IOFixture : test::Test
{
    hls_io *io = nullptr;

    void init() final { io = hls_io_New( nullptr, 0 ); }
    void release() final
    {
        io->ops.release( io );
        free( io );
    }
};

TEST_F( IOFixture, Init )
{
    ASSERT_TRUE( io );
    ASSERT_EQ( io->size, 0u );
}

TEST_F( IOFixture, SimpleSmallRead )
{
    static constexpr size_t SIZE = 0x10;

    block_t *block = block_Alloc( SIZE );
    memset( block->p_buffer, 0x42, SIZE );

    io->ops.consume_block( io, block );

    void *ctxt = io->ops.new_reading_context( io );
    ASSERT_TRUE( ctxt );

    std::array<uint8_t, SIZE> buffer = { 0 };
    const ssize_t ret = io->ops.read( ctxt, buffer.data(), SIZE );
    io->ops.release_reading_context( ctxt );
    ASSERT_EQ( ret, SIZE );

    for ( const uint8_t v : buffer )
        ASSERT_EQ( v, 0x42 );
}

TEST_F( IOFixture, ComplexSmallRead )
{
    static constexpr size_t SIZE = 2;

    block_t *block = block_Alloc( SIZE );
    memcpy( block->p_buffer, ( uint8_t[2] ){ 11, 55 }, SIZE );

    io->ops.consume_block( io, block );

    void *ctxt = io->ops.new_reading_context( io );
    ASSERT_TRUE( ctxt );

    uint8_t first_val = 0;
    const ssize_t first_read = io->ops.read( ctxt, &first_val, 1 );
    uint8_t second_val = 0;
    const ssize_t second_read = io->ops.read( ctxt, &second_val, 1 );
    io->ops.release_reading_context( ctxt );

    ASSERT_EQ( first_read, 1 );
    ASSERT_EQ( first_val, 11 );
    ASSERT_EQ( second_read, 1 );
    ASSERT_EQ( second_val, 55 );
}

TEST_F( IOFixture, ComplexBigRead )
{
    static constexpr size_t SIZE = 3333;

    block_t *block = block_Alloc( SIZE );
    memset( block->p_buffer, 0x42, SIZE / 2 );
    memset( block->p_buffer + SIZE / 2, 0x43, SIZE / 2 + 1 );

    io->ops.consume_block( io, block );

    void *ctxt = io->ops.new_reading_context( io );
    ASSERT_TRUE( ctxt );

    std::array<uint8_t, SIZE / 2> first_val = { 0 };
    const ssize_t first_read = io->ops.read( ctxt, first_val.data(), first_val.size() );
    std::array<uint8_t, SIZE / 2 + 1> second_val = { 0 };
    const ssize_t second_read = io->ops.read( ctxt, second_val.data(), second_val.size() );
    io->ops.release_reading_context( ctxt );

    ASSERT_EQ( first_read, SIZE / 2 );
    for ( const auto e : first_val )
        ASSERT_EQ( e, 0x42 );
    ASSERT_EQ( second_read, SIZE / 2 + 1 );
    for ( const auto e : second_val )
        ASSERT_EQ( e, 0x43 );
}

TEST_F( IOFixture, TwoBlocksRead )
{
    static constexpr size_t FIRST_SIZE = 0x10;
    static constexpr size_t SECOND_SIZE = 0x20;

    block_t *fb = block_Alloc( FIRST_SIZE );
    memset( fb->p_buffer, 0x42, FIRST_SIZE );

    block_t *sb = block_Alloc( SECOND_SIZE );
    memset( sb->p_buffer, 0x43, SECOND_SIZE );

    fb->p_next = sb;

    io->ops.consume_block( io, fb );

    void *ctxt = io->ops.new_reading_context( io );
    ASSERT_TRUE( ctxt );

    std::array<uint8_t, FIRST_SIZE - 5> first_val = { 0 };
    const ssize_t first_read = io->ops.read( ctxt, first_val.data(), first_val.size() );
    std::array<uint8_t, SECOND_SIZE + 5> second_val = { 0 };
    const ssize_t second_read = io->ops.read( ctxt, second_val.data(), second_val.size() );
    io->ops.release_reading_context( ctxt );

    ASSERT_EQ( first_read, FIRST_SIZE - 5 );
    for ( const auto e : first_val )
        ASSERT_EQ( e, 0x42 );
    ASSERT_EQ( second_read, SECOND_SIZE + 5 );
    for ( auto i = 0u; i < second_val.size(); ++i )
        ASSERT_EQ( second_val.at( i ), i < 5 ? 0x42 : 0x43 );
}

int main() { return test::TestPool::run_all( test::Logger() ) ? EXIT_SUCCESS : EXIT_FAILURE; }
