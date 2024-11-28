/**
 * @file emscripten.c
 * @brief Emscripten webgl video output for VLC media player
 */
/*****************************************************************************
 * Copyright © 2020 VLC authors and VideoLAN
 *
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
# include <config.h>
#endif

#include <cstdarg>
#include <new>
#include <string>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_window.h>
#include <vlc_vout_display.h>
#include <vlc_opengl.h>

#include "./opengl/vout_helper.h"
#include "opengl/renderer.h"

#include "emscripten/common.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#undef typeof // FIXME Remove once val::typeof is finally removed
#include <emscripten/val.h>
#include <webgl/webgl2.h>
// eglGetProcAddress
#include <EGL/egl.h>

static const struct vlc_window_operations ops = {
    //TODO: Implement canvas operations
    //vlc_window_ReportSize() should be called from here
};

//MAIN_THREAD_
//EM_JS(emscripten::EM_VAL, GetCanvasSize, (), {
//    let canvas = document.getElementById('canvas');
//    if (!canvas)
//        return Emval.bindValue([]);
//    return Emval.bindValue([canvas.width, canvas.height]);
//});

typedef struct emscripten_gl_sys_t
{
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context;
    vlc_thread_t worker;
} emscripten_gl_sys_t;

static int OpenWindow(vlc_window_t *wnd)
{
    wnd->type = VLC_WINDOW_TYPE_EMSCRIPTEN_WEBGL;
    wnd->ops = &ops;
    wnd->handle.canvas = "canvas";

    //emscripten::val size = GetCanvasSize();
    //std::vector<int> vec_size = emscripten::vecFromJSArray(size);
    //if (vec_size.size() != 2)
    //    return VLC_EGENERIC;

    //vlc_window_ReportSize(wnd, vec_size[0], vec_size[1]);
    vlc_window_ReportSize(wnd, 1280, 720);
    return VLC_SUCCESS;
}

static void *GetProcAddress(vlc_gl_t *gl, const char *name)
{
    VLC_UNUSED(gl);

    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

static int MakeCurrent(vlc_gl_t *gl)
{
    auto sys = static_cast<emscripten_gl_sys_t*>( gl->sys );

    if (emscripten_webgl_make_context_current(sys->context) != EMSCRIPTEN_RESULT_SUCCESS)
        return VLC_EGENERIC;
    return VLC_SUCCESS;
}

static void ReleaseCurrent(vlc_gl_t *gl)
{
    VLC_UNUSED(gl);
    emscripten_webgl_make_context_current(0);
}

static void Swap(vlc_gl_t *gl)
{
    /**
     * There is no way to commit/swapbuffer from an offscreen canvas, so instead
     * we render the offscreen canvas onto a bitmap and let the actual canvas
     * display that bitmap from the main thread.
     */
    VLC_UNUSED(gl);
    EM_ASM({
        let bitmap = Module.offscreenCanvas.transferToImageBitmap();
        Module.voutMsgPort.postMessage({
            customCmd: 'commitFrame',
            bitmap: bitmap,
        }, [bitmap]);
    });
}

static int InitRenderer(vlc_gl_t *gl)
{
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);
    return VLC_SUCCESS;
}

static void RenderNext(vlc_gl_t *gl)
{
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);

}

static void RequestChange(vlc_gl_t *gl)
{
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);

}

static void Resize(vlc_gl_t *gl, unsigned w, unsigned h)
{
    VLC_UNUSED(gl);
    VLC_UNUSED(w);
    VLC_UNUSED(h);
}

EM_ASYNC_JS(void, getVoutMessagePort, (), {
    let p = new Promise((resolve, reject) => {
        let listener = function(e) {
            let msg = e.data;
            if (msg.customCmd == 'getVoutMessagePortResult') {
                Module.voutMsgPort = msg['msgPort'];
                resolve();
                self.removeEventListener('message', listener);
            };
        };
        self.addEventListener('message', listener);
        self.postMessage({
            customCmd: 'getVoutMessagePort'
        });
    });
    await p;
});

EM_JS(int, createGlContext, (int width, int height), {
    Module.offscreenCanvas = new OffscreenCanvas(width, height);
    Module.glCtx = Module.offscreenCanvas.getContext('webgl2');

    return GL.registerContext(Module.glCtx, {
        antialias: false
    });
});

static void Close (vlc_gl_t *gl)
{
    auto sys = static_cast<emscripten_gl_sys_t*>( gl->sys );
    delete sys;
}

static void RunloopRender(void *opaque)
{
    auto gl = static_cast<vlc_gl_t*>(opaque);
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);

    vlc_gl_ReportRender(gl, 800, 600);
}

struct emscripten_startup {
    vlc_gl_t *gl;
    unsigned width;
    unsigned height;
    vlc_sem_t wait_sem;
};

static void OpenglInitialize(vlc_gl_t *gl, unsigned width, unsigned height)
{
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);
    vlc_window_t *wnd = gl->surface;

    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.majorVersion=2;
    attr.minorVersion=0;
    attr.explicitSwapControl = 1;

    MAIN_THREAD_EM_ASM({
        if (Module.canvasCtx === undefined) {
            let canvasName = UTF8ToString($1);
            Module.canvasCtx = document.getElementById(canvasName).getContext('2d');
        }
        function onVoutMessage(msg) {
            let data = msg['data'];
            if (data.customCmd == 'commitFrame') {
                let bmp = data['bitmap'];
                Module.canvasCtx.drawImage(bmp, 0, 0);
                bmp.close();
            }
        }
        let w = Module.PThread.pthreads[$0];
        w.addEventListener('message', function (e) {
            let msg = e['data'];
            if (msg.customCmd == 'getVoutMessagePort') {
                let msgChannel = new MessageChannel();
                msgChannel.port1.onmessage = onVoutMessage;
                w.postMessage({
                    customCmd: 'getVoutMessagePortResult',
                    msgPort: msgChannel.port2,
                }, [msgChannel.port2]);
            }
        });
    }, pthread_self(), wnd->handle.canvas ? wnd->handle.canvas : "canvas");

    getVoutMessagePort();
    sys->context = createGlContext(width, height);

    if (!sys->context) {
        //goto error;
        //TODO
    }
    // Check that the WebGL context is valid
    if (emscripten_webgl_make_context_current(sys->context) != EMSCRIPTEN_RESULT_SUCCESS) {
        emscripten_log(EM_LOG_CONSOLE, "failed to make context current");
        //goto error;
        // TODO
    }

    vlc_gl_ReportInit(gl);

    //// Release the context
    //emscripten_webgl_make_context_current(0);
    wnd->handle.em_context = sys->context;
}

static void* OpenglWorkerRunloop(void *opaque)
{
    auto startup = static_cast<struct emscripten_startup *>(opaque);

    vlc_gl_t *gl = startup->gl;
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);

    OpenglInitialize(gl, startup->width, startup->height);
    return NULL;
}

static int Open (vlc_gl_t *gl, unsigned width, unsigned height, const struct vlc_gl_cfg *cfg)
{
    (void)width, (void)height;
    (void)cfg;

    vlc_window_t *wnd = gl->surface;
    if (wnd->type != VLC_WINDOW_TYPE_EMSCRIPTEN_WEBGL)
        return VLC_ENOTSUP;

    emscripten_gl_sys_t *sys;

    gl->sys = sys = new (std::nothrow)emscripten_gl_sys_t;
    if (!sys)
        return VLC_ENOMEM;
    
    struct emscripten_startup startup = {
        .gl = gl,
        .width = width,
        .height = height,
    };
    vlc_sem_init(&startup.wait_sem, 0);

    if (vlc_clone(&sys->worker, OpenglWorkerRunloop, &startup) != VLC_EGENERIC)
    {
        delete sys ;
        gl->sys = NULL;
        return VLC_EGENERIC;
    }

    vlc_sem_wait(&startup.wait_sem);
    //EM_ASM({
    //    let w = Module.PThread.pthreads[$0];
    //}, pthread_self();

    // Implement gl routines:
    static const struct vlc_gl_operations ops = []{
        struct vlc_gl_operations ops {};
        ops.async_mode.init_renderer = InitRenderer;
        ops.async_mode.render_next = RenderNext;
        ops.async_mode.request_change = RequestChange;
        //.make_current = MakeCurrent,
        //.release_current = ReleaseCurrent,
        ops.resize = Resize;
        ops.swap = Swap;
        ops.get_proc_address = GetProcAddress;
        ops.close = Close;
        return ops;
    }();
    gl->ops = &ops;

    return VLC_SUCCESS;

error:
    Close(gl);
    return VLC_EGENERIC;
}

/*
 * Module descriptor
 */
vlc_module_begin()
    set_shortname(N_("Emscripten Window"))
    set_description(N_("Emscripten drawing area"))
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout window", 10)
    set_callbacks(OpenWindow, nullptr)

    add_submodule ()
    set_shortname("Emscripten GL")
    set_description(N_("Emscripten extension for OpenGL"))
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("opengl es2", 50)
    set_callback(Open)
    add_shortcut("em_webgl")
vlc_module_end()

