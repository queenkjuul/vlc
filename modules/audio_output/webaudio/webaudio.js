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

mergeInto(LibraryManager.library, {
    webaudio_getSampleRate: function() {
        if (Module.webaudioContext === undefined)
            return 0;
        return Module.webaudioContext.sampleRate;
    },
    webaudio_getChannels: function() {
        if (Module.webaudioContext === undefined)
            return 0;
        return Module.webaudioChannels;
    },
    webaudio_destroy: function() {
        Module.webaudioContext.close();
        Module.webaudioContext = undefined;
    },
    webaudio_init_async: async function(latency) {
        switch (latency) {
            case 0:
                latency = 'playback';
                break;
            case 1:
                latency = 'interactive';
                break;
            case 2: 
                latency = 'balanced';
                break;
            default:
                console.error(`Invalid latency setting: ${latency}`);
                return -1;
        }

        var audioCtx = new AudioContext({
            latencyHint: latency,
        });

        audioCtx.suspend();

        const path = Module.locateFile("audio-worklet-processor.js");
        try {
            await audioCtx.audioWorklet.addModule(path);
        } catch (error) {
            console.error(`Couldn't add worklet module: ${error}`);
	        return -1;
        }
        Module.webaudioContext = audioCtx;
        return 0;
    },
    webaudio_init__deps: ['webaudio_init_async'],
    webaudio_init__async: true,
    webaudio_init: function(latency) {
        return Asyncify.handleAsync(async () => { return await _webaudio_init_async(latency); });
    },

    webaudio_start_async: async function(channels, webaudio_buffer) {
        if (Module.webaudioContext === undefined) {
            console.error("No audio context");
            return -1;
        }

        const is_paused = new Int32Array(wasmMemory.buffer, webaudio_buffer, 1);
        is_paused[0] = 0;
        webaudio_buffer += 4;
        const head = new Int32Array(wasmMemory.buffer, webaudio_buffer, 1);
        webaudio_buffer += 4;
        const tail = new Int32Array(wasmMemory.buffer, webaudio_buffer, 1);
        webaudio_buffer += 4;
        const can_write = new Int32Array(wasmMemory.buffer, webaudio_buffer, 1);
        can_write[0] = 1;
        webaudio_buffer += 4;
        const STORAGE_SIZE = 1024 * 1024;
        const storage = new Float32Array(wasmMemory.buffer, webaudio_buffer, STORAGE_SIZE / 4);

        const msg = {
            type: 'recv-audio-queue',
            is_paused,
            head,
            tail,
            can_write,
            storage,
        };
        
        let audioCtx = Module.webaudioContext;
        if (audioCtx.destination.maxChannelCount < channels) {
            console.warn("Max number of channels of the browser is ", audioCtx.destination.maxChannelCount);
            channels = audioCtx.destination.maxChannelCount;
        }

        console.log("OUTPUT channels " + channels);
        const node = new AudioWorkletNode(audioCtx, 'worklet-processor', {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [channels],
        });

        const response = new Promise(function(resolve, reject) {
            node.port.onmessage = function(e) {
                if (e.data === 'ready') {
                    console.log('successfully constructed AudioWorkletProcessor');
                    resolve();
                }
                else if (e.data === 'error') {
                    reject();
                    return;
                }
            }
        });
        node.port.postMessage(msg);
        await response;
        
        node.connect(audioCtx.destination);
        console.log('connected');
        Module.webaudioNode = node; 
        Module.webaudioChannels = channels;
        Module.webaudioContext.resume();
    },
    webaudio_start__deps: ['webaudio_start_async'],
    webaudio_start__async: true,
    webaudio_start: function(channels, webaudio_buffer) {
        return Asyncify.handleAsync(async () => { return await _webaudio_start_async(channels, webaudio_buffer); });
    },

    webaudio_stop: function() {
        if (Module.webaudioContext === undefined) {
            console.error("No audio context");
            return -1;
        }

        Module.webaudioNode.port.postMessage({
            type: 'stop',
        });

        Module.webaudioContext.suspend();
        Module.webaudioNode.disconnect(Module.webaudioContext.destination);
        Module.webaudioNode = undefined;
    }
})
