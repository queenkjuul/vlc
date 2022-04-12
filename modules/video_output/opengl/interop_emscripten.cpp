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
#include "../emscripten/common.h"

#include <emscripten.h>
#include <emscripten/em_js.h>
#include <emscripten/val.h>
#include <emscripten/html5_webgl.h>

struct EmscriptenInterop
{
    struct
    {
        PFNGLBINDTEXTUREPROC BindTexture;
    } gl;
};

static int
tc_emscripten_op_allocate_textures(const struct vlc_gl_interop *interop, GLuint *textures,
                          const GLsizei *tex_width, const GLsizei *tex_height)
{
    (void) tex_width; (void) tex_height;
    assert(textures[0] != 0);

    return VLC_SUCCESS;
}

EM_ASYNC_JS(void, bindVideoFrame, (int pictureId), {
    let frame = await Module.awaitFrame(pictureId);

    let glCtx = Module.glCtx;
    glCtx.texImage2D(glCtx.TEXTURE_2D, 0, glCtx.RGBA, frame.codedWidth, frame.codedHeight, 0,
                glCtx.RGBA, glCtx.UNSIGNED_BYTE, frame);
})

static int
tc_emscripten_op_update(const struct vlc_gl_interop *interop, GLuint *textures,
               const GLsizei *tex_width, const GLsizei *tex_height,
               picture_t *pic, const size_t *plane_offset)
{
    auto sys = static_cast<EmscriptenInterop *>( interop->priv );

    sys->gl.BindTexture(interop->tex_target, textures[0]);

    auto pictureId = reinterpret_cast<uintptr_t>( pic->p_sys );
    bindVideoFrame(pictureId);

    return VLC_SUCCESS;
}

extern "C"
{
EMSCRIPTEN_KEEPALIVE int getDecoderWorker(vlc_video_context* vctx)
{
    auto wcCtx = static_cast<webcodec_context*>(
                vlc_video_context_GetPrivate(vctx, VLC_VIDEO_CONTEXT_WEBCODEC));
    if (!vctx)
        return 0;
    return wcCtx->decoder_worker;
}
}

EM_JS(void, transferMessagePort, (vlc_video_context* vctx), {
    function onDecoderMessage(msg) {
        let data = msg['data'];
        if (data.customCmd == 'displayFrame') {
            let pictureId = data.pictureId;
            let pictureIdx = pictureId % 32;
            let frame = data['frame'];
            console.log('SINK Received frame ' + pictureId );
            if ( Module.glConv.promiseResolvers[pictureIdx] ) {
                /* The interop is already waiting for this frame, resolve the promise */
                console.log('SINK Renderer is already waiting for frame ' + pictureId );
                Module.glConv.promiseResolvers[pictureIdx]( frame );
            } else {
                /* The frame hasn't arrived to the interop yet, queue it */
                console.log('SINK No resolver, queuing frame ' + pictureId );
                Module.glConv.frameQueue[pictureIdx] = frame;
            }
        }
    };
    Module.msgChannel = new MessageChannel();
    Module.msgChannel.port1.onmessage = onDecoderMessage;
    let workerId = _getDecoderWorker(vctx);
    if (workerId == 0)
        return;
    self.postMessage({
        customCmd: 'transferMessagePort',
        targetThread: workerId,
        transferList: [Module.msgChannel.port2],
    }, [Module.msgChannel.port2]);
});

EM_JS(void, initGlConvWorker, (int maxPictures), {
    Module.glConv = {};
    Module.glConv.promiseResolvers = [];
    Module.glConv.frameQueue = [];
    Module.glConv.lastFrame = {
        pictureId: -1,
        frame: undefined
    };

    Module.awaitFrame = async function(pictureId) {
        console.log('CONSUME Fetching frame ' + pictureId);
        if (Module.glConv.lastFrame.pictureId == pictureId) {
            console.log('Reusing last frame');
            return Module.glConv.lastFrame.frame;
        }
        let pictureIndex = pictureId % 32;
        let p = new Promise((resolve, reject) => {
            if ( Module.glConv.frameQueue[pictureIndex] ) {
                console.log('CONSUME Frame ', pictureId, '(index ', pictureIndex,
                            ') was already queued');
                let frame = Module.glConv.frameQueue[pictureIndex];
                resolve(frame);
                Module.glConv.frameQueue[pictureIndex] = undefined;
            } else {
                console.log('CONSUME Frame ', pictureId, '(index ', pictureIndex,
                            ') not available yet; exposing resolver' );
                Module.glConv.promiseResolvers[pictureIndex] = resolve;
            }
        });
        let frame = await p;
        Module.glConv.promiseResolvers[pictureId] = undefined;
        if (Module.glConv.lastFrame.frame)
            Module.glConv.lastFrame.frame.close();
        Module.glConv.lastFrame.frame = frame;
        Module.glConv.lastFrame.pictureId = pictureId;
        console.log('CONSUME got frame');
        return frame;
    }
})

EM_JS(void, closeMessagePort, (void), {
    if (Module.msgChannel)
        delete Module.msgChannel;
});

static void
Close(struct vlc_gl_interop *)
{
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

    static const struct vlc_gl_interop_ops ops = {
        .allocate_textures = tc_emscripten_op_allocate_textures,
        .update_textures = tc_emscripten_op_update,
        .close = Close,
    };
    interop->ops = &ops;
    initGlConvWorker(WEBCODEC_MAX_PICTURES);
    transferMessagePort(interop->vctx);

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
    interop->fmt_out.i_rmask = 0xFF0000;
    interop->fmt_out.i_gmask = 0x00FF00;
    interop->fmt_out.i_bmask = 0x0000FF;
    interop->priv = sys;

    interop->fmt_out.orientation = ORIENT_VFLIPPED;

    return VLC_SUCCESS;
}

EM_ASYNC_JS(void, CopyFrameToBuffer, (int pictureId, emscripten::EM_VAL infoHandle), {
    let info = Emval.toValue(infoHandle);
    let frame = await Module.awaitFrame(pictureId);
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
    auto pictureId = reinterpret_cast<uintptr_t>(src->p_sys);

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

    CopyFrameToBuffer( pictureId, info.as_handle() );

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
    transferMessagePort(filter->vctx_in);

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
        set_callback_video_converter(OpenConverter, 10)
vlc_module_end ()
