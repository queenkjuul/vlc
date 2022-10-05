/*****************************************************************************
 * interop_emscripten.cpp: OpenGL Emscripten/Webcodec opaque converter
 *****************************************************************************
 * Copyright (C) 2021 VLC authors and VideoLAN
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

//#ifndef __EMSCRIPTEN__
//# error this file must be built with emscripten
//#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>

#include "interop.h"
#include "gl_common.h"
#include <cassert>
#undef typeof
#include "../emscripten/common.h"

#include <emscripten.h>
#include <emscripten/em_js.h>
#undef typeof // FIXME Remove once val::typeof is finally removed
#include <emscripten/val.h>
#include <emscripten/html5_webgl.h>

struct EmscriptenInterop
{
    struct
    {
        PFNGLBINDTEXTUREPROC BindTexture;
    } gl;

    picture_t *lastPicture;
};

static int
tc_emscripten_op_allocate_textures(const struct vlc_gl_interop *interop, GLuint *textures,
                          const GLsizei *tex_width, const GLsizei *tex_height)
{
    (void)interop; (void) tex_width; (void) tex_height;
    assert(textures[0] != 0);

    return VLC_SUCCESS;
}

EM_ASYNC_JS(void, bindVideoFrame, (int pictureIdx, int decoderId), {
    let frame = await Module.awaitFrame(pictureIdx, decoderId);
    if (!frame)
    {
        return;
    }

    let glCtx = Module.glCtx;
    glCtx.texImage2D(glCtx.TEXTURE_2D, 0, glCtx.RGBA, frame.displayWidth, frame.displayHeight, 0,
                glCtx.RGBA, glCtx.UNSIGNED_BYTE, frame);
})

static int
tc_emscripten_op_update(const struct vlc_gl_interop *interop, GLuint *textures,
               const GLsizei *tex_width, const GLsizei *tex_height,
               picture_t *pic, const size_t *plane_offset)
{
    (void)tex_width; (void)tex_height; (void)plane_offset;
    auto sys = static_cast<EmscriptenInterop *>( interop->priv );

    sys->gl.BindTexture(interop->tex_target, textures[0]);

    auto picCtx = PictureContextPrivate(pic->context);
    auto vctx = static_cast<webcodec_context*>(
        vlc_video_context_GetPrivate( pic->context->vctx, VLC_VIDEO_CONTEXT_WEBCODEC ) );

    bindVideoFrame(picCtx->pictureIdx, vctx->decoder_worker);

    if (sys->lastPicture)
        picture_Release(sys->lastPicture);
    sys->lastPicture = picture_Hold(pic);

    if (sys->lastPicture)
        picture_Release(sys->lastPicture);
    sys->lastPicture = picture_Hold(pic);

    return VLC_SUCCESS;
}

//EMSCRIPTEN_KEEPALIVE emscripten::EM_VAL getDecoderWorker(vlc_video_context* vctx)
//{
//    auto wcCtx = static_cast<webcodec_context*>(
//                vlc_video_context_GetPrivate(vctx, VLC_VIDEO_CONTEXT_WEBCODEC));
//    return wcCtx->frame_pool_worker.as_handle();
//}

EM_JS(void, setupMessagePort, (vlc_video_context* vctx), {
    function onDecoderMessage(msg) {
        let data = msg['data'];
        if (data.customCmd == 'displayFrame') {
            let pictureIdx = data.pictureId;
            let frame = data['frame'];
            Module.glConv.frameResolver( frame );
        }
    };
    Module.msgChannel = new MessageChannel();
    Module.msgChannel.port1.onmessage = onDecoderMessage;
    self.postMessage({
        customCmd: 'connect',
        port: Module.msgChannel.port2,
    }, [Module.msgChannel.port2]);
});

EM_JS(void, initGlConvWorker, (int maxPictures), {
    Module.glConv = {};
    Module.glConv.lastFrame = {
        pictureIdx: -1,
        frame: undefined
    };

    Module.awaitFrame = async function(pictureIdx, decoderId) {
        if (Module.glConv.lastFrame.pictureIdx == pictureIdx) {
            return Module.glConv.lastFrame.frame;
        }
        let p = new Promise((resolve, reject) => {
            Module.glConv.frameResolver = resolve;
            Module.msgChannel.port1.postMessage({
                'customCmd': 'sendFrame',
                'pictureIdx': pictureIdx,
                'decoderId': decoderId
            });

        });
        let frame = await p;
        Module.glConv.frameResolver = undefined;
        if (Module.glConv.lastFrame.frame)
            Module.glConv.lastFrame.frame.close();
        Module.glConv.lastFrame.frame = frame;
        Module.glConv.lastFrame.pictureIdx = pictureIdx;
        return frame;
    }
})

EM_JS(void, closeMessagePort, (void), {
    if (Module.msgChannel)
        delete Module.msgChannel;
});

static void
Close(struct vlc_gl_interop *interop)
{
    auto *sys = static_cast<EmscriptenInterop *>( interop->priv );

    if (sys->lastPicture != NULL)
        picture_Release(sys->lastPicture);
    closeMessagePort();
}

static int
Open(vlc_object_t *obj)
{
    auto interop = reinterpret_cast<vlc_gl_interop*>( obj );

    if (interop->fmt_in.i_chroma != VLC_CODEC_WEBCODEC_OPAQUE)
        return VLC_EGENERIC;

    EmscriptenInterop *sys = (decltype(sys))
        vlc_obj_malloc(VLC_OBJECT(interop), sizeof *sys);
    if (sys == NULL)
        return VLC_EGENERIC;
    sys->gl.BindTexture = (decltype(sys->gl.BindTexture))
        vlc_gl_GetProcAddress(interop->gl, "glBindTexture");
    if (sys->gl.BindTexture == NULL)
        return VLC_EGENERIC;

    sys->lastPicture = NULL;

    static const struct vlc_gl_interop_ops ops = {
        .allocate_textures = tc_emscripten_op_allocate_textures,
        .update_textures = tc_emscripten_op_update,
        .close = Close,
    };
    interop->ops = &ops;
    initGlConvWorker(WEBCODEC_MAX_PICTURES);

    MAIN_THREAD_EM_ASM({
        const WEBCODEC_MAX_PICTURES = 32;
        const thread_id = $0;
        let w = Module.PThread.pthreads[thread_id];

        const connect = msg => {
            const port = msg.port;
            port.onmessage = e => {
                if (e.data.customCmd !== 'sendFrame')
                    return;

                const decoderId = e.data.decoderId;
                const picIndex = e.data.pictureIdx;
                console.assert(picIndex >= 0 && picIndex < WEBCODEC_MAX_PICTURES);
                const frame = Module.framePools[decoderId][picIndex];
                if (frame === undefined) {
                    /* We may receive a late message after a flush, in which case
                     * we still need to reply to the interop, but we don't have
                     * anything to provide to it
                     */
                    port.postMessage({
                        customCmd: 'displayFrame',
                        frame: undefined,
                        pictureId: picIndex
                    });
                    return;
                }

                port.postMessage({
                    customCmd: 'displayFrame',
                    frame: frame,
                    pictureId: picIndex,
                }, [frame.clone()]);

            };
        };

        w.addEventListener('message', e => {
            const LISTENERS = {
                'connect': connect,
            };

            const listener = LISTENERS[e.data.customCmd];
            if (listener !== undefined)
                listener(e.data);
        });
    }, pthread_self());

    setupMessagePort(interop->vctx);

    interop->tex_target = GL_TEXTURE_2D;
    interop->fmt_out.i_chroma = VLC_CODEC_RGBA;
    interop->fmt_out.space = COLOR_SPACE_UNDEF;
    interop->tex_count = 1;
    interop->texs[0] = vlc_gl_interop::vlc_gl_tex_cfg{
            /*.w =*/ { 1, 1 },
            /*.h =*/ { 1, 1 },
            /*.internal =*/ GL_RGBA,
            /*.format = */GL_RGBA,
            /*.type = */GL_UNSIGNED_BYTE,
    };
    interop->priv = sys;

    interop->fmt_out.orientation = ORIENT_VFLIPPED;

    return VLC_SUCCESS;
}

EM_ASYNC_JS(void, CopyFrameToBuffer, (int pictureIdx, emscripten::EM_VAL infoHandle), {
    let info = Emval.toValue(infoHandle);
    let frame = await Module.awaitFrame(pictureIdx);
    let copyOpts = {
        rect: frame.codedRect,
        layout: info.layout
    };
    await frame.copyTo(info.buffer, copyOpts);
});

static picture_t* vlc_webcodec_UploadVideoFrame(filter_t* filter, picture_t* src)
{
    auto pic = filter_NewPicture(filter);
    if (unlikely(pic == nullptr))
    {
        picture_Release(src);
        return nullptr;
    }

    auto pictureIdx = PictureContextPrivate(src->context)->pictureIdx;

    size_t picSize = 0;
    auto info = emscripten::val::object();
    auto planesLayout = emscripten::val::array();
    for ( auto i = 0; i < pic->i_planes; ++i )
    {
        auto p = pic->p[i];
        auto layout = emscripten::val::object();
        layout.set( "offset", picSize );
        layout.set( "stride", p.i_pitch );
        planesLayout.call<void>( "push", std::move( layout ) );
        picSize += p.i_lines * p.i_pitch;
    }

    info.set( "buffer", emscripten::typed_memory_view( picSize, pic->p[0].p_pixels ) );
    info.set( "layout", std::move( planesLayout ) );

    CopyFrameToBuffer( pictureIdx, info.as_handle() );

    pic->date = src->date;
    picture_Release(src);
    return pic;
}

static void vlc_webcodec_CloseConverter(filter_t*)
{
    closeMessagePort();
}

static const struct vlc_filter_operations video_frame_to_sw_ops = {
    .filter_video = vlc_webcodec_UploadVideoFrame,
    .close = vlc_webcodec_CloseConverter,
};

static int OpenConverter(filter_t* filter)
{
    if (filter->fmt_in.video.i_height != filter->fmt_out.video.i_height
     || filter->fmt_in.video.i_width != filter->fmt_out.video.i_width
     || filter->fmt_in.video.orientation != filter->fmt_out.video.orientation
     || filter->fmt_in.video.i_chroma != VLC_CODEC_WEBCODEC_OPAQUE
     || filter->fmt_out.video.i_chroma != VLC_CODEC_I420 )
    {
        return VLC_EGENERIC;
    }
    initGlConvWorker(WEBCODEC_MAX_PICTURES);
    setupMessagePort(filter->vctx_in);

    filter->ops = &video_frame_to_sw_ops;

    return VLC_SUCCESS;
}

vlc_module_begin ()
    set_description("Emscripten OpenGL SurfaceTexture converter")
    set_capability("glinterop", 1)
    set_callback(Open)
    set_subcategory(SUBCAT_VIDEO_VOUT)

    add_submodule()
        set_subcategory(SUBCAT_VIDEO_VFILTER)
        set_callback_video_converter(OpenConverter, 1000)
vlc_module_end ()
