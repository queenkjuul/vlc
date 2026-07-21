/*****************************************************************************
 * chromecast_common.h: Chromecast common code between modules for vlc
 *****************************************************************************
 * Copyright © 2015-2016 VideoLAN
 *
 * Authors: Adrien Maglo <magsoft@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Steve Lhomme <robux4@videolabs.io>
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

#ifndef VLC_CHROMECAST_COMMON_H
#define VLC_CHROMECAST_COMMON_H

#include <vlc_input.h>

# ifdef __cplusplus
extern "C" {
# endif

#define CC_SHARED_VAR_NAME "cc_sout"

/* HLS rolling-window parameters for the cast sout chain. Shared between the
 * sout (which builds the hls{} chain string from them) and the demux filter
 * (which derives its pre-playback pacing cap from them) so the two can never
 * disagree about the size of the segment window. */
#define CC_HLS_SEG_LEN_SEC      5
#define CC_HLS_MAX_SEG_LEN_SEC  10
#define CC_HLS_NUM_SEG          4

/* Target bitrate for transcoded audio, in kbps. AAC-LC stereo at 192k is
 * transparent enough for the cases that reach the encoder at all (the
 * passthrough codecs are handled without re-encoding). */
#define CC_AUDIO_BITRATE_KBPS   192

/* State of the receiver, as last reported by its MEDIA_STATUS messages. */
enum cc_device_state
{
    CC_DEVICE_OK,     /* Nothing to report, keep feeding. */
    CC_DEVICE_RETRY,  /* The load failed and a fallback is armed. Consumed by
                       * the call reporting it, so it is reported once. */
    CC_DEVICE_ERROR,  /* Errored or stopped, give up. */
    CC_DEVICE_ENDED,  /* Done playing what it had fetched. */
};

enum cc_input_event
{
    CC_INPUT_EVENT_EOF,
    CC_INPUT_EVENT_RETRY,
};

union cc_input_arg
{
    bool eof;
};

typedef void (*on_input_event_itf)( void *data, enum cc_input_event, union cc_input_arg );

typedef void (*on_paused_changed_itf)( void *data, bool );

typedef struct
{
    void *p_opaque;

    void (*pf_set_demux_enabled)(void *, bool enabled, on_paused_changed_itf, void *);

    vlc_tick_t (*pf_get_time)(void*);

    enum cc_device_state (*pf_get_device_state)(void*);

    void (*pf_send_input_event)(void*, enum cc_input_event, union cc_input_arg);

    void (*pf_set_pause_state)(void*, bool paused);

    void (*pf_set_meta)(void*, vlc_meta_t *p_meta);

    void (*pf_set_input_length)(void*, vlc_tick_t length);

    void (*pf_set_master_demux_time)(void*, vlc_tick_t);
    vlc_tick_t (*pf_get_master_demux_time)(void*);

} chromecast_common;

# ifdef __cplusplus
}
# endif

#endif // VLC_CHROMECAST_COMMON_H

