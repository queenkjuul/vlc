#include "cxx_test_helper.hpp"

//#include "../common.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <vlc_common.h>

#include <demux/mpeg/timestamps.h>
#include <meta_engine/ID3Meta.h>
#include <meta_engine/ID3Tag.h>
#include <vlc_block.h>

extern "C"
{
    block_t *Add_ID3( block_t *data );
}

template <vlc_tick_t DTS>
class ID3Fixture : public test::Test
{
    static constexpr auto BLOCK_SIZE = 0x10u;

    block_t *data;

    protected:
    void parse_id3()
    {
        data = Add_ID3( data );
        ASSERT_TRUE( data );

        static constexpr auto ID3_EXPECTED_SIZE = 73u;
        ASSERT_EQ( data->i_buffer, BLOCK_SIZE + ID3_EXPECTED_SIZE );

        const auto handler =
            []( uint32_t tag, const uint8_t *payload, size_t payload_size, void *timestamp )
        {
            if ( tag != VLC_FOURCC( 'P', 'R', 'I', 'V' ) )
                return VLC_EGENERIC;

            static constexpr char id[] = "com.apple.streaming.transportStreamTimestamp";
            if ( payload_size != 53 || memcmp( payload, id, sizeof( id ) ) )
                return VLC_EGENERIC;

            *static_cast<int64_t *>( timestamp ) = GetQWBE( payload + sizeof( id ) );
            return VLC_SUCCESS;
        };

        int64_t timestamp = -1;
        const size_t res = ID3TAG_Parse( data->p_buffer, data->i_buffer, handler, &timestamp );

        ASSERT_EQ( res, data->i_buffer - BLOCK_SIZE );
        ASSERT_EQ( timestamp, TO_SCALE_NZ( DTS ) );

        for ( auto i = 0u; i < BLOCK_SIZE; ++i )
        {
            ASSERT_EQ( data->p_buffer[res + i], 0x42 );
        }
    }

    public:
    void init() final
    {
        data = block_Alloc( BLOCK_SIZE );
        memset( data->p_buffer, 0x42, BLOCK_SIZE );
        data->i_dts = DTS;
    }

    void release() final { block_Release( data ); }
};

TEST_F(ID3Fixture<1>, ID3One){ parse_id3(); }
TEST_F(ID3Fixture<2>, ID3Two){ parse_id3(); }
TEST_F(ID3Fixture<323678643>, ID3Big){ parse_id3(); }

int main() { return test::TestPool::run_all( test::Logger() ) ? EXIT_SUCCESS : EXIT_FAILURE; }
