/*****************************************************************************
 * emscripten.c: Emscripten webaudio output
 *****************************************************************************
 * Copyright © 2022 VLC authors, VideoLAN and Videolabs
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>

#include <vlc_aout.h>
#include <vlc_atomic.h>
#include <vlc_plugin.h>

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include "webaudio/webaudio.h"

typedef struct aout_sys_t
{
    /* Shared Audio Buffer. */
    webaudio_buffer_t *sab;

    /* Audio device information shared with the main runtime. */
    unsigned channel_count;
    unsigned sample_rate;

    /* Task queue for the main runtime. */
    em_proxying_queue *proxying_queue;
    /* Signal main runtime task failure. */
    bool error;

    /* Web audio context initialization notifier. */
    atomic_uint context_ready;

    /* Required by volume.h. */
    bool soft_mute;
    float soft_gain;
} aout_sys_t;

#include "volume.h"

/*
  This function will clear the ring buffer in the shared array buffer.
 */
static void Flush(audio_output_t *aout)
{
    aout_sys_t *sys = aout->sys;

    emscripten_futex_wait(&sys->sab->can_write, 0, ATOMIC_WAIT_TIMEOUT);
    memset(sys->sab->storage, 0, sizeof(sys->sab->storage));
    atomic_store(&sys->sab->head, 0);
    atomic_store(&sys->sab->tail, 0);
}

static void Play(audio_output_t *aout, block_t *block, vlc_tick_t date)
{
    (void)date;
    aout_sys_t *sys = aout->sys;

    int8_t *data = (int8_t *)block->p_buffer;
    const size_t data_size = block->i_buffer;
    const uint32_t head = atomic_load(&sys->sab->head);
    const uint32_t tail = atomic_load(&sys->sab->tail);
    const uint32_t new_head = (head + data_size) % STORAGE_SIZE;

    if (new_head > tail)
    {
        /*
           The worklet processor keeps rendering  until tail matches head
           it will be notified by an Atomics.notify() from the process()
           callback.
        */
        emscripten_futex_wait(&sys->sab->can_write, 0, ATOMIC_WAIT_TIMEOUT);
    }
    webaudio_worklet_push(sys->sab, data, data_size);
    block_Release(block);
}

static void Pause(audio_output_t *aout, bool paused, vlc_tick_t date)
{
    (void)date;
    aout_sys_t *sys = aout->sys;

    if (paused == false)
        atomic_store(&sys->sab->is_paused, 0);
    else
        atomic_store(&sys->sab->is_paused, 1);
    Flush(aout);
}

/*
  To allow reusing the AudioWorklet, we stop the process() callback,
  from running by setting is_paused to 1.
*/
static void Stop(audio_output_t *aout)
{
    Flush(aout);
    aout_sys_t *sys = (aout_sys_t *)aout->sys;
    atomic_store(&sys->sab->is_paused, 1);
}

/** Fix channel mapping according to Webaudio API constraints.
 *
 * https://webaudio.github.io/web-audio-api/#ChannelOrdering
 */
static void FixChannelMapping(audio_sample_format_t *restrict fmt)
{
    switch (fmt->i_channels)
    {
        case 0:
        case 1:
            fmt->i_physical_channels = AOUT_CHAN_LEFT;
            break;
        case 2:
            fmt->i_physical_channels = AOUT_CHANS_2_0;
            break;
        case 3:
            /* WebAudio does not support 2.1 */
            fmt->i_physical_channels = AOUT_CHANS_2_0;
            fmt->i_channels = 2;
            break;
        case 4:
            fmt->i_physical_channels = AOUT_CHANS_4_0;
            break;
        case 5:
            /* WebAudio does not support 4.1 */
            fmt->i_physical_channels = AOUT_CHANS_4_0;
            fmt->i_channels = 4;
            break;
        default:
            /* WebAudio only supports up to 5.1 3F2R. */
            fmt->i_physical_channels = AOUT_CHANS_5_1;
            fmt->i_channels = 6;
            break;
    }
}

static void StartTask(void *opaque)
{
    aout_sys_t *sys = opaque;

    sys->error = webaudio_init(sys->sample_rate, WEBAUDIO_LATENCY_PLAYBACK) == -1;
    if (!sys->error)
    {
        sys->sample_rate = webaudio_getSampleRate();
        sys->error = webaudio_start(sys->channel_count, sys->sab) == -1;
    }
    if (!sys->error)
        sys->channel_count = webaudio_getChannels();

    atomic_store(&sys->context_ready, 1);
    vlc_atomic_notify_all(&sys->context_ready);
}

static int Start(audio_output_t *aout, audio_sample_format_t *restrict fmt)
{
    aout_sys_t *sys = aout->sys;

    FixChannelMapping(fmt);
    const unsigned nbChannels = aout_FormatNbChannels(fmt);

    aout_SoftVolumeStart(aout);
    if (( nbChannels == 0 ) || !AOUT_FMT_LINEAR(fmt))
        return VLC_EGENERIC;
    if (atomic_load(&sys->context_ready) == false)
    {
        sys->channel_count = nbChannels;
        sys->sample_rate = fmt->i_rate;

        const int ret = emscripten_proxy_sync(
            sys->proxying_queue,
            emscripten_main_runtime_thread_id(),
            StartTask, sys);
        if (!ret)
        {
            msg_Err(aout, "error: could not proxy Init to main runtime!");
            return VLC_EGENERIC;
        }

        // Wait until initialization ended.
        vlc_atomic_wait(&sys->context_ready, 0);

        if (sys->error || ((sys->channel_count == 0) && (sys->sample_rate == 0)))
            return VLC_EGENERIC;
    }

    fmt->i_format = VLC_CODEC_FL32;
    fmt->i_rate = sys->sample_rate;
    fmt->i_channels = sys->channel_count;
    // Ensure that physical channel mapping is correct.
    FixChannelMapping(fmt);

    atomic_store(&sys->sab->is_paused, 0);

    return VLC_SUCCESS;
}

static int Open(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;

    aout_sys_t *sys = malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    sys->proxying_queue = em_proxying_queue_create();
    if (unlikely(sys->proxying_queue == NULL))
        goto proxy_err;

    sys->sab = malloc(sizeof(*sys->sab));
    if (unlikely(sys->sab == NULL))
        goto sab_err;

    atomic_init(&sys->sab->is_paused, 0);
    atomic_init(&sys->sab->head, 0);
    atomic_init(&sys->sab->tail, 0);
    atomic_init(&sys->sab->can_write, 0);
    memset(sys->sab, 0, sizeof(webaudio_buffer_t));

    sys->channel_count = 0;
    sys->sample_rate = 0;
    sys->error = false;
    atomic_init(&sys->context_ready, 0);

    aout->start = Start;
    aout->stop = Stop;
    aout->play = Play;
    aout->pause = Pause;
    aout->flush = Flush;

    aout->sys = sys;

    /* We can't use the volume field in the shared array buffer, because
     * we need to have a lock, and we can't have atomic.wait in the
     * worklet, or atomics.store on a float value.
     */
    aout_SoftVolumeInit(aout);

    return VLC_SUCCESS;
sab_err:
    em_proxying_queue_destroy(sys->proxying_queue);
proxy_err:
    free(sys);
    return (-ENOMEM);
}

static void Close(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *)obj;
    aout_sys_t *sys = aout->sys;

    em_proxying_queue_destroy(sys->proxying_queue);
    free(sys->sab);
    free(sys);
}

// clang-format off
vlc_module_begin ()
    set_description( N_("Emscripten Worklet audio output") )
    set_shortname( "emworklet" )
    add_sw_gain ()
    set_capability( "audio output", 100 )
    set_subcategory( SUBCAT_AUDIO_AOUT )
    set_callbacks( Open, Close )
vlc_module_end ()
