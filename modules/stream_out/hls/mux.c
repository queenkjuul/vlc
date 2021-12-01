#include "common.h"

#include <vlc_block.h>
#include <vlc_sout.h>

typedef struct
{
    const char *mime;

    vlc_tick_t dts_delay;
    vlc_tick_t first_dts;
} sout_mux_sys_t;

static inline bool is_stream_added( const sout_mux_sys_t *sys ) { return sys->mime != NULL; }

static inline const char *get_track_mime( const vlc_fourcc_t codec )
{
    // clang-format off
    switch (codec)
    {
        case VLC_CODEC_MP4A: return "audio/aac";
        case VLC_CODEC_MP3: return "audio/mpeg";
        case VLC_CODEC_A52: return "audio/ac3";
        case VLC_CODEC_EAC3: return "audio/eac3";
        default: return NULL;
    }
    // clang-format on
}

static int Control( sout_mux_t *mux, int query, va_list args )
{
    const sout_mux_sys_t *sys = mux->p_sys;
    switch ( query )
    {
    case MUX_CAN_ADD_STREAM_WHILE_MUXING:
        *va_arg( args, bool * ) = false;
        return VLC_SUCCESS;

    case MUX_GET_MIME:
        *va_arg( args, char ** ) = strdup( sys->mime );
        return VLC_SUCCESS;

    default:
        return VLC_EGENERIC;
    }
}

static int AddStream( sout_mux_t *mux, sout_input_t *input )
{
    sout_mux_sys_t *sys = mux->p_sys;

    if ( is_stream_added( sys ) || input->p_fmt->i_cat != AUDIO_ES )
    {
        msg_Err( mux, "HLS packed audio only support one audio input." );
        return VLC_EGENERIC;
    }

    sys->mime = get_track_mime( input->p_fmt->i_codec );
    if ( sys->mime == NULL )
    {
        msg_Err( mux, "Unsupported audio codec" );
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

static void DelStream( sout_mux_t *mux, sout_input_t *input )
{
    sout_mux_sys_t *sys = mux->p_sys;

    sys->mime = NULL;

    (void)input;
}

static block_t *Add_ADTS( block_t *data, const es_format_t *fmt );

static int Mux( sout_mux_t *mux )
{
    sout_mux_sys_t *sys = mux->p_sys;

    if ( !is_stream_added( mux->p_sys ) )
        return VLC_EGENERIC;

    sout_input_t *input = mux->pp_inputs[0];

    vlc_fifo_Lock( input->p_fifo );
    const size_t block_count = vlc_fifo_GetCount( input->p_fifo );
    vlc_fifo_Unlock( input->p_fifo );

    for ( size_t i = 0; i < block_count; ++i )
    {
        block_t *data = block_FifoGet( input->p_fifo );

        if (sys->first_dts == VLC_TICK_INVALID ||  sys->first_dts > data->i_dts)
            sys->first_dts = data->i_dts;

        data->i_pts -= sys->first_dts - sys->dts_delay;
        data->i_dts -= sys->first_dts - sys->dts_delay;

        if ( input->fmt.i_codec == VLC_CODEC_MP4A )
            data = Add_ADTS( data, input->p_fmt );

        sout_AccessOutWrite( mux->p_access, data );
    }

    return VLC_SUCCESS;
}

int MuxOpen( vlc_object_t *this )
{
    sout_mux_t *mux = (sout_mux_t *)this;

    sout_mux_sys_t *sys = calloc( 1, sizeof( *sys ) );
    if ( unlikely( sys == NULL ) )
        return VLC_ENOMEM;

    static const char *const mux_options[] = { "dts-delay", NULL };
    config_ChainParse( mux, MUX_CFG_PREFIX, mux_options, mux->p_cfg );

    sys->dts_delay = VLC_TICK_FROM_MS( var_GetInteger( mux, MUX_CFG_PREFIX "dts-delay" ) );

    mux->p_sys = sys;

    mux->pf_control = Control;
    mux->pf_addstream = AddStream;
    mux->pf_delstream = DelStream;
    mux->pf_mux = Mux;

    return VLC_SUCCESS;
}

void MuxClose( vlc_object_t *this )
{
    sout_mux_t *mux = (sout_mux_t *)this;

    free( mux->p_sys );
}

// Extracted from mux/mpeg/ts.c
// TODO: unify both functions
static block_t *Add_ADTS( block_t *data, const es_format_t *fmt )
{
#define ADTS_HEADER_SIZE 7 /* CRC needs 2 more bytes */

    const uint8_t *p_extra = fmt->p_extra;

    if ( !data || fmt->i_extra < 2 || !p_extra )
        return data; /* no data to construct the headers */

    size_t frame_length = data->i_buffer + ADTS_HEADER_SIZE;
    int i_index = ( ( p_extra[0] << 1 ) | ( p_extra[1] >> 7 ) ) & 0x0f;
    int i_profile = ( p_extra[0] >> 3 ) - 1; /* i_profile < 4 */

    if ( i_index == 0x0f && fmt->i_extra < 5 )
        return data; /* not enough data */

    int i_channels = ( p_extra[i_index == 0x0f ? 4 : 1] >> 3 ) & 0x0f;

    block_t *p_new_block = block_Realloc( data, ADTS_HEADER_SIZE, data->i_buffer );
    uint8_t *p_buffer = p_new_block->p_buffer;

    /* fixed header */
    p_buffer[0] = 0xff;
    p_buffer[1] = 0xf1; /* 0xf0 | 0x00 | 0x00 | 0x01 */
    p_buffer[2] = ( i_profile << 6 ) | ( ( i_index & 0x0f ) << 2 ) | ( ( i_channels >> 2 ) & 0x01 );
    p_buffer[3] = ( i_channels << 6 ) | ( ( frame_length >> 11 ) & 0x03 );

    /* variable header (starts at last 2 bits of 4th byte) */

    int i_fullness = 0x7ff; /* 0x7ff means VBR */
    /* XXX: We should check if it's CBR or VBR, but no known implementation
     * do that, and it's a pain to calculate this field */

    p_buffer[4] = frame_length >> 3;
    p_buffer[5] = ( ( frame_length & 0x07 ) << 5 ) | ( ( i_fullness >> 6 ) & 0x1f );
    p_buffer[6] = ( ( i_fullness & 0x3f ) << 2 ) /* | 0xfc */;

    return p_new_block;
}
