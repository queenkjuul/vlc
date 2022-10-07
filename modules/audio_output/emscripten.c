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
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_atomic.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>

#include "webaudio/webaudio.h"

typedef struct aout_sys_t
{
    /*
      Shared structure, that allows the AudioWorkletProcessor (AWP)
      to interact with vlc's decoder thread.

      is_paused : control the AWP process loop,
      head : write index in the ring buffer,
      tail : read index in the ring buffer,
      can_write : control if you can write to the ring buffer or not,
      storage[STORAGE_SIZE] : ring buffer,
    */
    webaudio_buffer_t *sab;
    /*
      channels, and sample rate are needed here to allow dispatching
      them in an opaque pointer to the main thread.
    */
    unsigned channels;
    unsigned sample_rate;
    /*
      We don't want to create an AudioContext each time we play the media,
      and we can't init the context in Open() because we don't have
      access to audio_sample_format_t.
    */
    atomic_uint init;

    // required by volume.h
    bool soft_mute;
    float soft_gain;
} aout_sys_t;

#include "volume.h"

/*
  This function will clear the ring buffer in the shared array buffer.
 */
static void Flush(audio_output_t *aout)
{
    aout_sys_t *sys = (aout_sys_t *) aout->sys;
    emscripten_futex_wait(&sys->sab->can_write, 0, ATOMIC_WAIT_TIMEOUT);
    memset(sys->sab->storage, 0, sizeof(sys->sab->storage));
    atomic_store(&sys->sab->head, 0);
    atomic_store(&sys->sab->tail, 0);
}

static void Play(audio_output_t *aout, block_t *block, vlc_tick_t date)
{
    VLC_UNUSED(date);
    aout_sys_t *sys = (aout_sys_t *) aout->sys;
    int8_t *data = (int8_t *) block->p_buffer;
    size_t data_size = block->i_buffer;
    uint32_t head = atomic_load(&sys->sab->head);
    uint32_t tail = atomic_load(&sys->sab->tail);
    uint32_t new_head = (head + data_size) % STORAGE_SIZE;

    if (new_head > tail) {
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
    VLC_UNUSED(date);
    aout_sys_t * sys = (aout_sys_t *) aout->sys;

    if (paused == false)
        atomic_store(&sys->sab->is_paused, 0);
    else
        atomic_store(&sys->sab->is_paused, 1);
    Flush(aout);
}

//static int Time_Get(audio_output_t *aout, vlc_tick_t *delay)
//{
//    return aout_TimeGetDefault(aout, delay);
//}

static void Close(vlc_object_t *obj)
{
    audio_output_t *aout = (audio_output_t *) obj;
    aout_sys_t *sys = (aout_sys_t *) aout->sys;

    free(sys->sab);
    free(sys);
}

/*
  To allow reusing the AudioWorklet, we stop the process() callback,
  from running by setting is_paused to 1.
*/
static void Stop(audio_output_t *aout)
{
    Flush(aout);
    aout_sys_t * sys = (aout_sys_t *) aout->sys;
    atomic_store(&sys->sab->is_paused, 1);
}

/*
  Init Function will set the AudioContext, and try to get the sample rate and
  number of channels. 

  If they are not supported, we fallback to the supported ones.

  It can only run in the main thread.
*/
static void Init(void *userData)
{
    audio_output_t *aout = (audio_output_t *) userData;
    aout_sys_t *sys = (aout_sys_t *) aout->sys;

    webaudio_init(sys->sample_rate, sys->channels, sys->sab, WEBAUDIO_LATENCY_PLAYBACK);
    unsigned rate = webaudio_getSampleRate();
    unsigned channels = webaudio_getChannels();

    if ((rate == 0) || (channels == 0)) {
        sys->sample_rate = 0;
        sys->channels = 0;
        msg_Err(aout, "error: broken webaudio device, could not get a valid sample rate or channelNumber");
        return;
    }

    if (rate != sys->sample_rate)
        sys->sample_rate = rate;
    if (channels != sys->channels)
        sys->channels = channels;

    atomic_store(&sys->init, 1);
    vlc_atomic_notify_all(&sys->init);
}

static int Start(audio_output_t *aout, audio_sample_format_t *restrict fmt)
{
    aout_sys_t *sys = (aout_sys_t *) aout->sys;
    unsigned nbChannels = aout_FormatNbChannels(fmt);

    aout_SoftVolumeStart(aout);
    if (( nbChannels == 0 ) || !AOUT_FMT_LINEAR(fmt))
        return VLC_EGENERIC;
    fmt->i_format = VLC_CODEC_FL32;
    if (atomic_load(&sys->init) == false) {
        sys->channels = nbChannels;
        sys->sample_rate = fmt->i_rate;

        int ret = emscripten_dispatch_to_thread(
            emscripten_main_browser_thread_id(),
            EM_FUNC_SIG_VI,
            Init, 0, aout );
        if (ret != 0) {
            msg_Err(aout, "error: could not dispatch Init to main thread!");
            return VLC_EGENERIC;
        }

        // Wait until initialization ended.
        vlc_atomic_wait(&sys->init, 0);

        if ((sys->channels == 0) && (sys->sample_rate == 0))
            return VLC_EGENERIC;

        // XXX: Still needs checks for correctness.
        // It looks like webaudio channels are treated in logical order.
        // As of today we didn't find any way to fetch the true channel mapping
        // of the browser so for now, lets remap naively in case of necessary
        // downmix.
        static const unsigned CHANNEL_MAPPING[] = {
            0,
            AOUT_CHAN_LEFT,
            AOUT_CHANS_2_0,
            AOUT_CHANS_2_1,
            AOUT_CHANS_4_0,
            AOUT_CHANS_5_0,
            AOUT_CHANS_5_1,
        };

        if (fmt->i_channels > sys->channels)
        {
            msg_Info(aout, "Channel remapping necessary: %u -> %u", fmt->i_channels, sys->channels);
            if (unlikely(sys->channels > ARRAY_SIZE(CHANNEL_MAPPING)))
            {
                msg_Err(aout, "Unsupported mapping (channel count: %u)", sys->channels);
                return VLC_EGENERIC;
            }
            fmt->i_physical_channels = CHANNEL_MAPPING[sys->channels];
        }
        fmt->i_channels = sys->channels;
        fmt->i_rate = sys->sample_rate;
    }
    atomic_store(&sys->sab->is_paused, 0);

    return VLC_SUCCESS;
}

/*
  Allocate the Shared Array Buffer, and init aout_sys_t.
*/
static int Open(vlc_object_t *obj)
{
    audio_output_t * aout = (audio_output_t *) obj;

    aout_sys_t *sys = (aout_sys_t *) malloc(sizeof(aout_sys_t));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    aout->sys = sys;
    aout->start = Start;
    aout->stop = Stop;
    aout->play = Play;
    aout->pause = Pause;
    aout->flush = Flush;
    //aout->time_get = Time_Get;
    sys->channels = 0;
    sys->sample_rate = 0;
    atomic_init(&sys->init, 0);
    sys->sab = (webaudio_buffer_t *) malloc(sizeof(webaudio_buffer_t));
    atomic_init(&sys->sab->is_paused, 0);
    atomic_init(&sys->sab->head, 0);
    atomic_init(&sys->sab->tail, 0);
    atomic_init(&sys->sab->can_write, 0);
    if (unlikely (!sys->sab))
        return VLC_ENOMEM;    

    memset(sys->sab, 0, sizeof(webaudio_buffer_t));
    /*
      we can't use the volume field in the shared array buffer, because
      we need to have a lock, and we can't have atomic.wait in the
      worklet, or atomics.store on a float value.
    */
    aout_SoftVolumeInit(aout);

    return VLC_SUCCESS;
}

vlc_module_begin ()
    set_description( N_("Emscripten Worklet audio output") )
    set_shortname( "emworklet" )
    add_sw_gain ()
    set_capability( "audio output", 100 )
    set_subcategory( SUBCAT_AUDIO_AOUT )
    set_callbacks( Open, Close )
vlc_module_end ()
