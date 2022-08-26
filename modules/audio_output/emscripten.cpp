/*****************************************************************************
 * emscripten.c: audio output module using audio worklets
 *****************************************************************************
 * Copyright © 2020 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <atomic>

#include <assert.h>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>

#include <emscripten.h>
#include <emscripten/val.h>
#include <emscripten/bind.h>
#include <emscripten/html5.h>

#include <cstdint>
#include <stdlib.h>

#define STORAGE_SIZE 1024 * 1024
// Sample rate might change, and it would be good to be able to change it during playback.
#define AUDIO_WORKLET_SAMPLE_RATE 44100


using namespace emscripten;
namespace {
	EM_BOOL requestAnimationFrame_cb( double time, void *userData );

	typedef struct sound_buffer_t
	{
    // TODO - should be bool?
    std::atomic<uint32_t> is_paused;
    std::atomic<uint32_t> head;
    std::atomic<uint32_t> tail;
    std::atomic<uint32_t> can_write;
    std::atomic<uint32_t> volume;
    std::atomic<uint32_t> is_muted;
    int8_t storage[STORAGE_SIZE];

	} sound_buffer_t;

	class AWNodeWrapper {
	public:
		val context = val::undefined();
		val getCtx() const { return context; };
		void setCtx(val v_context) { context = v_context; };

		uintptr_t sab_ptr;
		uintptr_t getSabPtr() const { return sab_ptr; };
		void setSabPtr(uintptr_t p_sab) { sab_ptr = p_sab; };

		unsigned channels;
		unsigned getChannels() const { return channels; };
		void setChannels(int8_t chan) { channels = chan; };

		AWNodeWrapper(unsigned long sample_rate) {
			// Prepare audio context options
			val audio_ctx_options = val::object();
			audio_ctx_options.set("sampleRate", sample_rate);

			context = val::global("AudioContext").new_(audio_ctx_options);
			context.call<void>("suspend");
		}

		val operator()( val undefined_promise_argument ) {
			(val)undefined_promise_argument;

			// Prepare AWN Options
			val awn_options = val::object();
			val awn_opt_outputChannelCount = val::array();
			awn_opt_outputChannelCount.call<val>("push", channels);
			awn_options.set("outputChannelCount", awn_opt_outputChannelCount);
			awn_options.set("numberOfInputs", 0);
			awn_options.set("numberOfOutputs", 1);

			val AudioNode = val::global("AudioWorkletNode").new_(context, std::string("worklet-processor"), awn_options);
			AudioNode.set("channelCount", channels);

      val Uint32Array = val::global("Uint32Array");
      val Int32Array = val::global("Int32Array");
      val Float32Array = val::global("Float32Array");

      auto wasm_mem = val::module_property("wasmMemory")["buffer"];

			//Prepare postMessage message
			val msg = val::object();
			msg.set("type", std::string("recv-audio-queue"));

      msg.set("is_paused",
        Uint32Array.new_(wasm_mem, sab_ptr + offsetof(sound_buffer_t, is_paused), 1));
      msg.set("head",
        Uint32Array.new_(wasm_mem, sab_ptr + offsetof(sound_buffer_t, head), 1));
      msg.set("tail",
        Uint32Array.new_(wasm_mem, sab_ptr + offsetof(sound_buffer_t, tail), 1));
      msg.set("can_write",
        Int32Array.new_(wasm_mem, sab_ptr + offsetof(sound_buffer_t, can_write), 1));
      msg.set("volume",
        Int32Array.new_(wasm_mem, sab_ptr + offsetof(sound_buffer_t, volume), 1));
      msg.set("is_muted",
        Uint32Array.new_(wasm_mem, sab_ptr + offsetof(sound_buffer_t, is_muted), 1));

      uint32_t storage_capacity = STORAGE_SIZE / 4;
      msg.set("storage",
        Float32Array.new_(wasm_mem, sab_ptr + offsetof(sound_buffer_t, storage), storage_capacity));

			AudioNode["port"].call<val>("postMessage", msg);
			AudioNode.call<val>("connect", context["destination"]);

			emscripten_request_animation_frame_loop(requestAnimationFrame_cb, this);

			return val::undefined();
		}
	};

	EMSCRIPTEN_BINDINGS(AWWSCOPE) {
		class_<AWNodeWrapper>("awn_cb_wrapper")
			.constructor<int>()
			.property("context", &AWNodeWrapper::getCtx, &AWNodeWrapper::setCtx)
			.property("sab_ptr", &AWNodeWrapper::getSabPtr, &AWNodeWrapper::setSabPtr)
			.property("channels", &AWNodeWrapper::getChannels, &AWNodeWrapper::setChannels)
			.function("awn_call", &AWNodeWrapper::operator());
	};

	typedef struct aout_sys_t
	{
		sound_buffer_t *sab; // TODO - rename to sound_buff
		AWNodeWrapper *awn_inst;
		
		// let's see if it changes channels to 2 and samplerate to 44100;
	    unsigned i_channelCount = 0;
        unsigned long i_sampleRate = 48000;
	} aout_sys_t;

	EM_BOOL requestAnimationFrame_cb( double time, void *userData ) {
		VLC_UNUSED(time);
    // FIXME - this function seems to mix two different views on the
    // same memory, not sure why
		AWNodeWrapper *inst = reinterpret_cast<AWNodeWrapper *>(userData);
		uint32_t *sab = reinterpret_cast<uint32_t *>(inst->getSabPtr());
		val view = val(typed_memory_view(sizeof(sound_buffer_t), sab));
		val context = inst->getCtx();
		if ( view[0].as<int>() == 1 ) {
			context.call<val>("resume");
			sab[0] = 0;
			return EM_FALSE;
		}
		return EM_TRUE;
	}

	// careful when calling this, you cannot wait on any index
	// https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Atomics/wait
	void js_index_wait(sound_buffer_t *sab_ptr, int8_t index) {
		int32_t *buffer_view = reinterpret_cast<int32_t *>(sab_ptr);
		val buffer = val(typed_memory_view(STORAGE_SIZE, buffer_view));

		val::global("Atomics").call<val>("wait", buffer, index, 0);
	}

	void Flush( audio_output_t *aout )
	{
		aout_sys_t * sys = reinterpret_cast<aout_sys_t *>(aout->sys);
		memset(&sys->sab->storage, 0, sizeof(sys->sab->storage));
	}

	int Start( audio_output_t *aout, audio_sample_format_t *restrict fmt )
	{
		aout_sys_t *sys = reinterpret_cast<aout_sys_t *>(aout->sys);
		unsigned nbChannels = aout_FormatNbChannels(fmt);

		if (( nbChannels == 0 ) || !AOUT_FMT_LINEAR(fmt))
			return VLC_EGENERIC;
		fmt->i_format = VLC_CODEC_FL32;
		fmt->i_channels = sys->i_channelCount;
		fmt->i_rate = sys->i_sampleRate;

		// resume audio context (first start, it is paused when initialized)
		sys->sab->is_paused.store(1);

		return VLC_SUCCESS;
	}

	void Stop (audio_output_t *aout)
	{
		Flush(aout);
	}

	int audio_worklet_push (audio_output_t *aout, const int8_t *data, uint32_t data_size) {
		aout_sys_t *sys = reinterpret_cast<aout_sys_t *>(aout->sys);
		int8_t *sab_view = sys->sab->storage;
		uint32_t head = sys->sab->head.load();

		// TODO: check that we do not write on unconsumed data.
		if (head + data_size > STORAGE_SIZE) {
			// Copy the part of the data at the buffer end
			unsigned data_size_copy_end = STORAGE_SIZE - head;
			memcpy(sab_view + head, data, data_size_copy_end);
			head = 0;

			// Copy the part of the data at the buffer start
			unsigned data_size_copy_start = data_size - data_size_copy_end;
			memcpy(sab_view + head, data + data_size_copy_end, data_size_copy_start);
			head = data_size_copy_start;
		}
		else {
			memcpy(sab_view + head, data, data_size);
			head += data_size;
		}
		sys->sab->head.store(head);
		return 0;  // return success to indicate successful push.
	}

	void Play( audio_output_t *aout, block_t *block, vlc_tick_t date)
	{
		VLC_UNUSED(date);
		aout_sys_t *sys = reinterpret_cast<aout_sys_t *>(aout->sys);
		const int8_t* data = (int8_t *)block->p_buffer;
		size_t data_size = block->i_buffer;

		uint32_t head = sys->sab->head.load();
		uint32_t tail = sys->sab->tail.load();
		uint32_t new_head = (head + data_size) % STORAGE_SIZE;
		if (new_head > tail)
		{
			// the worklet processor keeps rendering  until tail matches head
			// it will be notified by an Atomics.notify() from the process() callback
			// FIXME - This is layout-dependent, which isn't ideal
			js_index_wait(sys->sab, 3);
		}

		audio_worklet_push(aout, data, data_size);
		block_Release(block);
	}

	void Pause( audio_output_t *aout, bool paused, vlc_tick_t date )
	{
		VLC_UNUSED(date);
		aout_sys_t * sys = reinterpret_cast<aout_sys_t *>(aout->sys);
		if (paused == false) {
			sys->sab->is_paused.store(0);
		}
		else {
			sys->sab->is_paused.store(1);
		}
		Flush(aout);
	}

	int Time_Get( audio_output_t *aout, vlc_tick_t *delay)
	{
		return aout_TimeGetDefault(aout, delay);
	}

	void Close( vlc_object_t *obj )
	{
		audio_output_t *aout = (audio_output_t *)obj;
		struct aout_sys_t *sys = reinterpret_cast<struct aout_sys_t *>(aout->sys);

    // FIXME
		delete sys->awn_inst;
		free(sys->sab);
		free(sys);
	}

	int Volume_Set( audio_output_t *aout, float volume)
	{
		struct aout_sys_t *sys = reinterpret_cast<struct aout_sys_t *>(aout->sys);

		if (volume > 1.0f)
			volume = 1.0f;
		else if (volume < 0.0f)
			volume = 0.0f;
		// TODO: implement gain
		// Note: We store volume as an integer between 0..100 because
		// for some reason Float32Array doesn't allow atomic operations
		sys->sab->volume.store((int)(volume * 100));
		aout_VolumeReport(aout, volume);

		return 0;
	}

	int Mute_Set( audio_output_t *aout, bool mute)
	{
		struct aout_sys_t *sys = reinterpret_cast<struct aout_sys_t *>(aout->sys);

		sys->sab->is_muted.store(mute);
		aout_MuteReport(aout, mute);

		return 0;
	}


	int Open( vlc_object_t *obj )
	{
		audio_output_t * aout = (audio_output_t *) obj;

		/* Allocate structures */
		aout_sys_t *sys = reinterpret_cast<aout_sys_t *>(malloc( sizeof( *sys ) ));
		if( unlikely(sys == NULL) )
			return VLC_ENOMEM;

		aout->sys = sys;
		aout->start = Start;
		aout->stop = Stop;
		aout->play = Play;
		aout->pause = Pause;
		aout->flush = Flush;
		aout->time_get = Time_Get;
		aout->volume_set = Volume_Set;
		aout->mute_set = Mute_Set;
		
        // Audio output is breaking when we implement i_sampleRate in here.
		sys->awn_inst = new AWNodeWrapper(AUDIO_WORKLET_SAMPLE_RATE);
		sys->sab = (sound_buffer_t*)malloc(sizeof(sound_buffer_t));

		if ( unlikely(sys->sab == NULL) )
			return VLC_ENOMEM;
		memset(sys->sab, 0, sizeof(sound_buffer_t));
		sys->sab->volume = 100;

		val webaudio_context = sys->awn_inst->getCtx();

        //These EM_ASM values needs to be re-written with Emval. (WiP)
        EM_ASM(
        let actx = new AudioContext();
        console.log("maxChannelCount: " + actx.destination.maxChannelCount);
        );

        sys->i_channelCount = EM_ASM_INT(
        let actx = new AudioContext();
        console.log("channelCount: " + actx.destination.channelCount);
        return actx.destination.maxChannelCount;
        );
        msg_Info(aout, "i_channelCount: %d", sys->i_channelCount);

        sys->i_sampleRate = EM_ASM_INT(
        let actx = new AudioContext();
        console.log("sampleRate: " + actx.destination.context.sampleRate );
        return actx.destination.context.sampleRate;
        );
        msg_Info(aout, "i_sampleRate: %lu", sys->i_sampleRate);

		// Prepare audioWorkletProcessor blob
		val document = val::global("document");
		val script = document.call<val>("createElement", std::string("SCRIPT"));
		script.set("type", std::string("worklet"));
		std::string processorStr = "class Processor extends AudioWorkletProcessor { \
	constructor() { \
		super(); \
		this.port.onmessage = e => { \
			if (e.data.type === 'recv-audio-queue') { \
				this.is_paused = e.data.is_paused; \
				this.head = e.data.head; \
				this.tail = e.data.tail; \
				this.can_write = e.data.can_write; \
				this.volume = e.data.volume; \
				this.is_muted = e.data.is_muted; \
				this.storage = e.data.storage; \
			} else { \
				throw 'unexpected.'; \
			} \
		}; \
	} \
	process(inputs, outputs, parameters) { \
		const output = outputs[0]; \
		const nbChannels = output.length; \
		const nbSamples = output[0].length; \
		if (this.head.buffer.byteLength == 0) { \
			throw new Error('wasmMemory grew'); \
		} \
		var head = Atomics.load(this.head, 0) / 4; \
		var tail = Atomics.load(this.tail, 0) / 4; \
		var i = 0; \
		var volume = Atomics.load(this.volume, 0) / 100; \
		if (Atomics.load(this.is_paused, 0) != 0 || Atomics.load(this.is_muted, 0) != 0) { \
			volume = 0; \
		} \
		while (tail != head && i < nbSamples) \
		{ \
			for (let c = 0; c < nbChannels; ++c) { \
				output[c][i] = this.storage[tail] * volume; \
				tail++; \
				if (tail == this.storage.length) { \
					tail = 0; \
				} \
			} \
			i++; \
		} \
		Atomics.store(this.tail, 0, tail * 4); \
		Atomics.store(this.can_write, 0, 1); \
		Atomics.notify(this.can_write, 0);   \
		return true; \
	} \
} \
registerProcessor('worklet-processor', Processor);";
		script.set("innerText", processorStr);
		val ProcessorTextArray = val::array();
		ProcessorTextArray.call<val>("push", script["innerText"]);
		val BlobObject = val::object();
		BlobObject.set("type", std::string("application/javascript"));
		val WorkletModuleUrl = val::global("URL").call<val>("createObjectURL", val::global("Blob").new_(ProcessorTextArray, BlobObject));

		// Prepare audioWorkletProcessor callback
		val cb_caller = val::module_property("awn_cb_wrapper").new_(sys->i_sampleRate);
		cb_caller.set("context", val(webaudio_context));
		cb_caller.set("sab_ptr", val(reinterpret_cast<uintptr_t>(sys->sab)));
		cb_caller.set("channels", val(sys->i_channelCount));
		val awn_caller = cb_caller["awn_call"];
		val awn_cb = awn_caller.call<val>("bind", cb_caller);

		// start audio worklet (since the context is suspended, sound won't start now
		// Since the WebAudio Context cannot be created in a worker, we create
		// it in the main_thread and use the SAB to signal it when we want it to start
		webaudio_context["audioWorklet"].call<val>("addModule", WorkletModuleUrl).call<val>("then", awn_cb);

		return VLC_SUCCESS;
	}
}

vlc_module_begin ()
	set_description( N_("Emscripten Worklet audio output") )
	set_shortname( "emworklet" )
	set_capability( "audio output", 100 )
	set_subcategory( SUBCAT_AUDIO_AOUT )
	set_callbacks( Open, Close )
vlc_module_end ()
