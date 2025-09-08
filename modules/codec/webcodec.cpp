/*****************************************************************************
 * webcodec.cpp: Decoder module using browser provided codec implementations
 *****************************************************************************
 * Copyright © 2021 VLC authors and VideoLAN
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

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_codec.h>
#include <vlc_threads.h>
#include <vlc_cxx_helpers.hpp>
#include <vlc_block.h>
#include <vlc_tick.h>
#include <vlc_picture.h>

#include "../video_output/emscripten/common.h"

#include <emscripten/emscripten.h>
#undef typeof // FIXME Remove once val::typeof is finally removed
#include <emscripten/val.h>
#include <emscripten/bind.h>
#include <emscripten/em_js.h>
#include <emscripten/wire.h>
#include <memory>
#include <functional>
#include <cstdint>
#include <queue>

using emval = emscripten::val;

struct decoder_sys_t
{
    vlc_thread_t th;
    std::queue<block_t*> blocks;
    vlc::threads::mutex mutex;

    vlc_video_context* vctx;
    bool first_frame = true;
};

extern "C"
{
EMSCRIPTEN_KEEPALIVE picture_t* tryGetPictureFromPool(decoder_t* dec)
{
    auto sys = static_cast<decoder_sys_t*>( dec->p_sys );
    auto vctx = static_cast<webcodec_context*>(
                vlc_video_context_GetPrivate(sys->vctx, VLC_VIDEO_CONTEXT_WEBCODEC) );
    return picture_pool_Get(vctx->pool);
}

EMSCRIPTEN_KEEPALIVE bool updateVideoOutput(decoder_t* dec)
{
    auto sys = static_cast<decoder_sys_t*>( dec->p_sys );
    if ( decoder_UpdateVideoOutput( dec, sys->vctx ) )
    {
        msg_Err( dec, "Failure during UpdateVideoOutput! FIXME" );
        return false;
    }
    return true;
}

EMSCRIPTEN_KEEPALIVE void queuePicture(void* ctx, picture_t* pic, int32_t timestamp)
{
    auto dec = static_cast<decoder_t*>(ctx);
    pic->date = VLC_TICK_FROM_US(timestamp);
    pic->b_progressive = true;
    decoder_QueueVideo(dec, pic);
    return PictureContextPrivate(pic->context)->pictureIdx;
}

EMSCRIPTEN_KEEPALIVE pthread_t getVlcDecoderWorkerThread(decoder_t* dec)
{
    auto sys = static_cast<decoder_sys_t*>( dec->p_sys );
    return sys->th.handle;
}

EMSCRIPTEN_KEEPALIVE void releaseBlock( block_t* block )
{
    block_Release( block );
}

EMSCRIPTEN_KEEPALIVE void decodeBlock( block_t* block )
{
    auto chunkType = emval::global("EncodedVideoChunk");
    auto chunkCfg = emval::object();

    static bool first = true;

    if ( first == true )
    {
        chunkCfg.set( "type", "key" );
        first = false;
    }
    else
        chunkCfg.set( "type", (block->i_flags & BLOCK_FLAG_TYPE_I) ? "key" : "delta" );
    auto timestamp = block->i_pts ? block->i_pts : block->i_dts;
    chunkCfg.set( "timestamp", (long int)US_FROM_VLC_TICK( timestamp ) );
    //if ( block->i_length > 0 )
    //    chunkCfg.set( "duration", (long int)US_FROM_VLC_TICK( block->i_length ) );
    chunkCfg.set( "data", emscripten::typed_memory_view( block->i_buffer, block->p_buffer ) );
    auto chunk = chunkType.new_( std::move( chunkCfg ) );

    /* We can't return a javascript object from the stack back to the JS caller
     * as the object would be destroyed when falling out of scope.
     * We can't also pass an object by reference as the only way to get a
     * emscripten::val instance is to take ownership from the javascript object
     * so we couldn't use it back in the JS
     * As a work around, we access the JS decoder instance straight from the C++ code
     */
    EM_ASM({Module.decoder.decode( Emval.toValue( $0 ) ); }, chunk.as_handle());
}

}

EM_ASYNC_JS(bool, probeConfig, (emscripten::EM_VAL cfg), {
    var decoderCfg = Emval.toValue(cfg);
    var res = await VideoDecoder.isConfigSupported(decoderCfg).catch((err) => {
        console.log(err);
        return {'supported': false};
    });
    return res['supported'];
});

static emval getDecoderConfig( decoder_t* dec, bool includeExtraData )
{
    auto decoderConfig = emval::object();
    switch( dec->fmt_in->i_codec )
    {
    case VLC_CODEC_VP8:
        decoderConfig.set( "codec", "vp8" );
        break;
    case VLC_CODEC_VP9:
        decoderConfig.set( "codec", "vp09.*" );
        break;
    case VLC_CODEC_H264:
    {
        char codec[12];
        snprintf(codec, sizeof(codec), "avc1.%.2X%.2X%.2X", dec->fmt_in->i_profile,
                 0, dec->fmt_in->i_level);
        decoderConfig.set( "codec", codec );

        //decoderConfig.set("codec", "avc1.64000b" );
        break;
    }
    case VLC_CODEC_AV1:
        decoderConfig.set( "codec", "av01" );
        break;
    default:
        return emval::undefined();
    }
    auto a = decoderConfig["codec"].as<std::string>();

    printf("mdr %s %dx%d %dx%d fps: %d\n", a.c_str(), dec->fmt_in->video.i_width,
           dec->fmt_in->video.i_height, dec->fmt_in->video.i_visible_width,
           dec->fmt_in->video.i_visible_height, dec->fmt_in->video.i_frame_rate / dec->fmt_in->video.i_frame_rate_base);
    if (dec->fmt_in->video.i_width)
        decoderConfig.set( "codedWidth", dec->fmt_in->video.i_width );
    if (dec->fmt_in->video.i_height)
        decoderConfig.set( "codedHeight", dec->fmt_in->video.i_height );
    if (dec->fmt_in->video.i_visible_width)
        decoderConfig.set( "displayAspectWidth", dec->fmt_in->video.i_visible_width );
    if (dec->fmt_in->video.i_visible_height)
        decoderConfig.set( "displayAspectHeight", dec->fmt_in->video.i_visible_height );
    if (dec->fmt_in->video.i_visible_height)
        decoderConfig.set( "framerate", dec->fmt_in->video.i_frame_rate / dec->fmt_in->video.i_frame_rate_base);
    //decoderConfig.set( "codedWidth", 1920 );
    //decoderConfig.set( "codedHeight", 1080 );
    //decoderConfig.set( "displayAspectWidth", 1920 );
    //decoderConfig.set( "displayAspectHeight", 1080 );
    decoderConfig.set( "optimizeForLatency", false );
    if ( includeExtraData )
    {
        if ( dec->fmt_in->i_extra > 0 )
        {
            decoderConfig.set( "description",
                               emscripten::typed_memory_view(
                                    dec->fmt_in->i_extra,
                                   static_cast<uint8_t*>( dec->fmt_in->p_extra ) )
                               );
        }
    }
    return decoderConfig;
}

EM_JS(emscripten::EM_VAL, initDecoderJS, (void* decoder, emscripten::EM_VAL decCfgHandle), {
    function onInteropMessage(msg) {
        let data = msg['data'];
        if (data['customCmd'] == 'sendFrame') {
            let picIdx = data['pictureIdx'];
            let frame = Module.framesReady[picIdx];
            Module.framesReady[picIdx] = undefined;
            if (!frame) {
                /* We may receive a late message after a flush, in which case
                 * we still need to reply to the interop, but we don't have
                 * anything to provide to it
                 */
                Module.voutPort.postMessage({
                    customCmd: 'displayFrame',
                    frame: undefined,
                    pictureId: picIdx
                });
                return;
            }
            Module.voutPort.postMessage({
                customCmd: 'displayFrame',
                frame: frame,
                pictureId: picIdx,
              }, [frame]);
        }
    }

    async function getVoutMessagePort() {
        let p = new Promise((resolve, reject) => {
            self.addEventListener('message', function(e) {
                let msg = e['data'];
                if (msg.customCmd == 'transferMessagePort') {
                    let port = msg['transferList'][0];
                    if (!port)
                        reject();
                    Module.voutPort = port;
                    Module.voutPort.onmessage = onInteropMessage;
                    resolve();
                }
            });
        });
        await p;
    }

    async function onDecoderWorkerMessage(msg) {
        const data = msg['data'];
        if (data['customCmd'] == 'decode') {
            let block = data['block'];
            _decodeBlock( block );
            _releaseBlock(block);
        } else if ( data['customCmd'] == 'flush' ) {
            await Module.decoder.flush();
            for ( let i = 0; i < Module.framesReady.length; ++i ) {
                if (Module.framesReady[i]) {
                    Module.framesReady[i].close();
                    Module.framesReady[i] = undefined;
                }
            }
            Module.msgChannel.port1.postMessage({
                customCmd: 'onFlushCompleted'
            });
        } else if ( data['customCmd'] == 'close' ) {
            Module.decoder.close();
        }
    }

    async function getPictureAsync(dec) {
        function getPicture(dec, resolve) {
            let pic = _tryGetPictureFromPool(dec);
            if (!pic) {
                setTimeout( function() {
                      getPicture(dec, resolve);
                  }, 1);
                return;
            }
            resolve(pic);
        }
        let p = new Promise((resolve) => {
            getPicture(dec, resolve);
        });
        return await p;
    }

    self.addEventListener('message', (e) => {
        let msg = e['data'];

        console.log('Received message in decoder worker');
        console.dir(msg);

        if (msg['customCmd'] == 'getDecoderWorkerMessagePort') {
            Module.msgChannel = new MessageChannel();
            Module.msgChannel.port1.onmessage = onDecoderWorkerMessage;
            self.postMessage({
                customCmd: 'transferMessagePort',
                targetThread: msg['replyTo'],
                transferList: [Module.msgChannel.port2],
            }, [Module.msgChannel.port2]);
        }
    });

    /* Store a handle to our decoder_t for later invocations */
    Module.webCodecCtx = decoder;

    /* Prepare an array containing the decoded frames, so the interop can query
     * then later */
    Module.framesReady = [];

    let initCfg = {
        'output': async function (frame) {
            /* Always update the video output format so we can ensure the vout
             * is created when we try to acquire its message port */
            if ( !_updateVideoOutput(Module.webCodecCtx) )
            {
                frame.close();
                return;
            }
            if (Module.voutPort === undefined)
                await getVoutMessagePort();
            let p = await getPictureAsync(Module.webCodecCtx);
            let picIdx = _queuePicture(Module.webCodecCtx, p, frame.timestamp);
            if ( Module.framesReady[picIdx] ) {
                /*
                 *  If we end up overriding a frame, it means it was dropped
                 *  by the vout before the interop asked for it, so we can close
                 *  it safely.
                 */
                console.log('Discarding dropped frame ', picIdx);
                Module.framesReady[picIdx].close();
            }
            Module.framesReady[picIdx] = frame;
        },
        'error': function(err) {
            console.log('Error while decoding: ');
            console.log(err);
        }
    };
    Module.decoder = new VideoDecoder( initCfg );
    let decCfg = Emval.toValue( decCfgHandle );
    Module.decoder.configure( decCfg );
});

static bool initDecoder( decoder_t* dec )
{
    auto decCfg = getDecoderConfig( dec, true );
    initDecoderJS( dec, decCfg.as_handle() );

    return true;
}

static void mainloop_tick()
{
}

static void* WebcodecDecodeWorker( void* arg )
{
    /*
     * We need to be able to yield back to the browser main loop after we
     * queue some blocks for decoding, otherwise the callback will never be
     * invoked.
     * In order to do so, we spawn a dedidated thread that will run a "main loop"
     * which consist of dequeuing a single block and passing it to webcodec.
     * After doing so, it yields back to the message loop and so on. If no
     * blocks are queued for decoding, we can block as there nothing else being
     * done from this worker.
     */
    auto dec = static_cast<decoder_t*>( arg );
    if ( !initDecoder( dec ) )
    {
        msg_Err( dec, "Failed to initialize decoder: FIXME" );
        return NULL;
    }
    auto sys = static_cast<decoder_sys_t*>( dec->p_sys );
    auto vctxPrivate = static_cast<webcodec_context*>(
            vlc_video_context_GetPrivate(sys->vctx, VLC_VIDEO_CONTEXT_WEBCODEC));
    vctxPrivate->decoder_worker = pthread_self();

    return NULL;
}

EM_ASYNC_JS(void, initDecoderWorkerMessagePort, (decoder_t* dec), {
    if (Module.decoderWorkerPort !== undefined) {
        return;
    }
    let workerMessagePortPromise = new Promise((resolve, reject) => {
        self.addEventListener('message', function(e) {
            let msg = e['data'];
            if (msg.customCmd == 'transferMessagePort') {
                let port = msg['transferList'][0];
                if (!port) {
                    console.log('No port provided, rejecting');
                    reject();
                }
                Module.decoderWorkerPort = port;
                resolve();
            }
        });
        self.postMessage({
            customCmd: 'getDecoderWorkerMessagePort',
            targetThread: _getVlcDecoderWorkerThread(dec),
            replyTo: _pthread_self()
        });
    });
    await workerMessagePortPromise;
    Module.decoderWorkerPort.onmessage = (e) => {
        let data = e['data'];
        if (data['customCmd'] == 'onFlushCompleted') {
            Module.flushPromiseResolver();
        }
    };
})

static int Decode( decoder_t* dec, block_t* block )
{
    if ( !block ) return VLCDEC_SUCCESS;

    auto sys = static_cast<decoder_sys_t*>( dec->p_sys );
    if (sys->first_frame && !(block->i_flags & VLC_FRAME_FLAG_TYPE_I) )
        return VLCDEC_SUCCESS;
    sys->first_frame = true;
    initDecoderWorkerMessagePort(dec);
    EM_ASM({
        Module.decoderWorkerPort.postMessage({
            customCmd: 'decode',
            block: $0
        });
    }, block);
    return VLCDEC_SUCCESS;
}

EM_ASYNC_JS(void, flushAsync, (), {
    let p = new Promise((r) => {
        Module.flushPromiseResolver = r;
        Module.decoderWorkerPort.postMessage({
            customCmd: 'flush'
        });
    });
    await p;
    Module.flushPromiseResolver = undefined;
});

static void Flush( decoder_t* dec )
{
    initDecoderWorkerMessagePort(dec);
    flushAsync();
}

static int Open( vlc_object_t* obj )
{
    auto dec = reinterpret_cast<decoder_t*>(obj);

    if ( dec->fmt_in->i_cat != VIDEO_ES )
        return VLC_EGENERIC;

    auto decoderType = emval::global("VideoDecoder");
    if ( !decoderType.as<bool>() )
    {
        msg_Err( obj, "Can't get VideoDecoder type, webcodec is probably not "
                      "supported on this browser" );
        return VLC_EGENERIC;
    }
    auto sys = std::make_unique<decoder_sys_t>();
    dec->p_sys = sys.get();

    auto decoderConfig = getDecoderConfig( dec, false );
    if ( decoderConfig.isUndefined() )
    {
        msg_Err(obj,"Codec config not supported");
        return VLC_EGENERIC;
    }
    msg_Info(obj,"Codec config supported!");
    auto isSupported = probeConfig(decoderConfig.as_handle());

    if ( isSupported == false )
    {
        msg_Err( dec, "VideoDecoder doesn't support this configuration" );
        return VLC_EGENERIC;
    }

    if ( es_format_Copy( &dec->fmt_out, dec->fmt_in ) != VLC_SUCCESS )
        return VLC_ENOMEM;
    dec->fmt_out.i_codec = dec->fmt_out.video.i_chroma = VLC_CODEC_WEBCODEC_OPAQUE;

    auto dec_dev = decoder_GetDecoderDevice(dec);
    sys->vctx = vlc_video_context_Create(dec_dev, VLC_VIDEO_CONTEXT_WEBCODEC,
                                         sizeof(webcodec_context), nullptr);
    auto vctxPrivate = static_cast<webcodec_context*>(
            vlc_video_context_GetPrivate(sys->vctx, VLC_VIDEO_CONTEXT_WEBCODEC));
    new (vctxPrivate) webcodec_context();

    if ( webcodec_CreatePool(sys->vctx, &dec->fmt_out.video) != VLC_SUCCESS )
        return VLC_EGENERIC;

    if ( vlc_clone( &sys->th, &WebcodecDecodeWorker, dec ) != VLC_SUCCESS )
    {
        msg_Err( obj, "Failed to create webcodec thread" );
        return VLC_EGENERIC;
    }

    dec->pf_decode = &Decode;
    dec->pf_flush = &Flush;

    sys.release();
    return VLCDEC_SUCCESS;
}

static void Close( vlc_object_t* obj )
{
    auto *dec = reinterpret_cast<decoder_t*>(obj);
    auto sys = static_cast<decoder_sys_t*>( dec->p_sys );
    auto vctx = static_cast<webcodec_context*>(
                vlc_video_context_GetPrivate(sys->vctx, VLC_VIDEO_CONTEXT_WEBCODEC ) );
    picture_pool_Release(vctx->pool);
    EM_ASM({
        Module.decoderWorkerPort.postMessage({
            customCmd: 'close'
        });
    });
    delete sys;
}

static int
OpenDecDevice(vlc_decoder_device *device, vlc_window_t *)
{
    static const struct vlc_decoder_device_operations ops =
    {
        nullptr,
    };
    device->ops = &ops;
    device->type = VLC_DECODER_DEVICE_WEBCODEC;

    return VLC_SUCCESS;
}

vlc_module_begin ()
    set_description("Video decoder using browser provided implementation")
    set_subcategory(SUBCAT_INPUT_VCODEC)
    set_section(N_("Decoding"), NULL)
    set_capability("video decoder", 100)
    set_callbacks(Open, Close)
    add_shortcut("webcodec")

    add_submodule()
        set_callback_dec_device(OpenDecDevice, 1)
vlc_module_end ()
