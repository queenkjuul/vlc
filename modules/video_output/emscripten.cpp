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
#include <vlc_actions.h>
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


typedef struct emscripten_gl_sys_t
{
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context;
    vlc_thread_t worker;
    pthread_t worker_id;
    bool initialized;
    bool dirty;
    unsigned width;
    unsigned height;
    bool resize;

    vlc_sem_t sem_destroy;
} emscripten_gl_sys_t;

struct emscripten_wnd_sys {
    char *canvas;
    bool resize;
};

static void CloseWindow(vlc_window_t *wnd)
{
    auto *sys = static_cast<emscripten_wnd_sys*>(wnd->sys);
    free(sys->canvas);
    free(sys);
}

extern "C" 
{
EMSCRIPTEN_KEEPALIVE void requestResize(vlc_window_t *window, unsigned w, unsigned h) {
    auto *sys = static_cast<emscripten_wnd_sys*>(window->sys);
    printf("tweaking value %d %d\n", w, h);
    //sys->resize = true;
    vlc_window_ReportSize(window, w, h);
}
}

static int WindowEnable(vlc_window_t *wnd, const vlc_window_cfg_t *cfg)
{
    const unsigned w = MAIN_THREAD_EM_ASM_INT({
        return document.getElementById(UTF8ToString($0)).width;
    }, wnd->handle.canvas);
    const unsigned h = MAIN_THREAD_EM_ASM_INT({
        return document.getElementById(UTF8ToString($0)).height;
    }, wnd->handle.canvas);

    MAIN_THREAD_ASYNC_EM_ASM({
        const canvas = document.getElementById(UTF8ToString($0));
        const resizeObserver = new ResizeObserver((entries) => {
            console.log(`Size changed `);
            const w = entries[0].target.clientWidth;
            const h = entries[0].target.clientHeight;
            _requestResize($1, w, h);
        });
        resizeObserver.observe(canvas);
        
    }, wnd->handle.canvas, wnd);

    printf("window size %d %d\n", w, h);
    vlc_window_ReportSize(wnd, w, h);
    return VLC_SUCCESS;
}

static const struct vlc_window_operations ops = [] {
    struct vlc_window_operations o{};
    o.destroy = CloseWindow;
    o.enable = WindowEnable;
    return o;
}();

static int OpenWindow(vlc_window_t *wnd)
{
    char *canvas = var_InheritString(wnd, "drawable-canvas");
    assert(canvas);
    if (canvas == nullptr)
        return VLC_EINVAL;

    auto *sys =
        static_cast<emscripten_wnd_sys *>(malloc(sizeof(emscripten_wnd_sys)));

    sys->canvas = canvas;
    sys->resize = false;

    wnd->type = VLC_WINDOW_TYPE_EMSCRIPTEN_WEBGL;
    wnd->ops = &ops;
    wnd->sys = sys;
    wnd->handle.canvas = canvas;

    return VLC_SUCCESS;
}

static void *GetProcAddress(vlc_gl_t *gl, const char *name)
{
    VLC_UNUSED(gl);
    msg_Info(gl, "Trying to extract %s: %s", __func__, name);

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
{}


EM_ASYNC_JS(void, SwapForReal, (vlc_gl_t *gl), {
    let bitmap = Module.offscreenCanvas.transferToImageBitmap();
    await new Promise((resolve) => {
        setTimeout(() => {
            Module.voutMsgPort.postMessage({
                customCmd: 'commitFrame',
                bitmap: bitmap,
            }, [bitmap]);
            resolve();
        });
    });
})

static int InitRenderer(vlc_gl_t *gl)
{
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);
    MAIN_THREAD_EM_ASM({
        let w = Module.vouts[$0];
        w.msgPort.postMessage({
            customCmd: 'initRenderer' 
        })
    }, sys->worker_id);
    return VLC_SUCCESS;
}

static void RenderNext(vlc_gl_t *gl)
{
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);

    //vlc_window_t *wnd = gl->surface;
    //auto *wnd_sys = static_cast<emscripten_wnd_sys*>(wnd->sys);
    //if (wnd_sys->resize)
    //{
    //    const unsigned w = MAIN_THREAD_EM_ASM_INT({
    //        return document.getElementById(UTF8ToString($0)).width;
    //    }, wnd->handle.canvas);
    //    const unsigned h = MAIN_THREAD_EM_ASM_INT({
    //        return document.getElementById(UTF8ToString($0)).height;
    //    }, wnd->handle.canvas);

    //    printf("Report size %d %d\n", w, h);
    //    vlc_window_ReportSize(wnd, w, h);
    //    wnd_sys->resize = false;
    //}

    MAIN_THREAD_EM_ASM({
        let w = Module.vouts[$0];
        w.msgPort.postMessage({
            customCmd: 'renderNext' 
        })
    }, sys->worker_id);
}

static void RequestChange(vlc_gl_t *gl)
{
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);

    MAIN_THREAD_EM_ASM({
        let w = Module.vouts[$0];
        w.msgPort.postMessage({
            customCmd: 'requestChange' 
        })
    }, sys->worker_id);
}

static void Resize(vlc_gl_t *gl, unsigned w, unsigned h)
{
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);

    MAIN_THREAD_EM_ASM({
        let w = Module.vouts[$0];
        w.msgPort.postMessage({
            customCmd: 'resize' ,
            width: $1,
            height: $2,
        })
    }, sys->worker_id, w, h);
}

EM_ASYNC_JS(void, getVoutMessagePort, (pthread_t worker_id), {
    let p = new Promise((resolve, reject) => {
        let listener = function(e) {
            let msg = e.data;
            if (msg.customCmd == 'getVoutMessagePortResult') {
                Module.voutMsgPort = msg['msgPort'];
                console.dir(Module.voutMsgPort);
                resolve();
                self.postMessage({ customCmd: 'voutMessagePortUnbind' });
                self.removeEventListener('message', listener);
            }
        };
        self.addEventListener('message', listener);
        self.postMessage({
            customCmd: 'getVoutMessagePort',
            worker_id: worker_id,
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

static void Close(vlc_gl_t *gl)
{
    auto sys = static_cast<emscripten_gl_sys_t*>( gl->sys );

    MAIN_THREAD_EM_ASM({
        window.removeEventListener("mousemove", Module.mouseEventListener);
        window.removeEventListener("mousedown", Module.mouseEventListener);
        window.removeEventListener("mouseup", Module.mouseEventListener);

        let w = Module.vouts[$0];
        w.msgPort.postMessage({
            customCmd: 'close'
        });
    }, sys->worker_id);
    // TODO: This is just sad. We can't do that for now, see the attached
    // commit message.
    //vlc_join(sys->worker, NULL);
    vlc_sem_wait(&sys->sem_destroy);

    delete sys;
}

extern "C" 
{
EMSCRIPTEN_KEEPALIVE void printhello(vlc_window_t *window) {
    vlc_window_ReportKeyPress(window, KEY_ENTER);
}
EMSCRIPTEN_KEEPALIVE void mousemove(vlc_window_t *window, int x, int y) {
    vlc_window_ReportMouseMoved(window, x, y);
}
EMSCRIPTEN_KEEPALIVE void mousedown(vlc_window_t *window) {
    vlc_window_ReportMousePressed(window, MOUSE_BUTTON_LEFT);
}
EMSCRIPTEN_KEEPALIVE void mouseup(vlc_window_t *window) {
    vlc_window_ReportMouseReleased(window, MOUSE_BUTTON_LEFT);
}
}

static void RunloopRender(void *opaque)
{
    auto gl = static_cast<vlc_gl_t*>(opaque);
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);

    int ret = EM_ASM_INT({
        if (!Module.vout.initialized)
            return -1;
        if (!Module.vout.dirty)
            return -1;
        Module.vout.dirty = false;
        return 0;
    });
    if (ret != 0)
        return;

    if (vlc_gl_ReportRender(gl, sys->width, sys->height))
        SwapForReal(gl);
}

struct emscripten_startup {
    vlc_gl_t *gl;
    unsigned width;
    unsigned height;
    vlc_sem_t wait_sem;
};

extern "C" {
EMSCRIPTEN_KEEPALIVE
void EmscriptenInitRenderer(vlc_gl_t *gl)
{
    vlc_gl_ReportInit(gl);
}

EMSCRIPTEN_KEEPALIVE
void EmscriptenRequestChange(vlc_gl_t *gl)
{
    vlc_gl_ApplyChanges(gl);
}

EMSCRIPTEN_KEEPALIVE
void EmscriptenKeepalivePop(void)
{
    emscripten_runtime_keepalive_pop();
}

EMSCRIPTEN_KEEPALIVE
void EmscriptenResize(vlc_gl_t *gl, unsigned width, unsigned height)
{
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);
    sys->width = width;
    sys->height = height;
}

EMSCRIPTEN_KEEPALIVE
void EmscriptenReportDestroy(vlc_gl_t *gl)
{
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);
    vlc_gl_ReportDestroy(gl);
    vlc_sem_post(&sys->sem_destroy);
}
}

static void OpenglInitialize(vlc_gl_t *gl, unsigned width, unsigned height)
{
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);
    vlc_window_t *wnd = gl->surface;

    sys->width = width;
    sys->height = height;
    sys->resize = false;

    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.majorVersion=2;
    attr.minorVersion=0;
    attr.explicitSwapControl = 1;

    MAIN_THREAD_EM_ASM({
        function onVoutMessage(msg) {
            let data = msg['data'];
            if (data.customCmd == 'commitFrame') {
                let bmp = data['bitmap'];
                let ctx = document.getElementById(UTF8ToString($1)).getContext('bitmaprenderer');
                ctx.transferFromImageBitmap(bmp, 0, 0);
           }
        }

        let w = Module.PThread.pthreads[$0];
        if (Module.vouts === undefined)
            Module.vouts = {};

        let workerMessageEvent = function(e) {
            let msg = e['data'];
            if (msg.customCmd == 'getVoutMessagePort') {
                console.log("getVoutMessagePort received for " + msg.worker_id + " from worker " + $0);
                if (msg.worker_id != $0)
                {
                    console.log("unattended getVoutMessagePort received for " + msg.worker_id + " from worker " + $0);
                    return;
                }
                let msgChannel = new MessageChannel();
                msgChannel.port1.onmessage = onVoutMessage;
                Module.vouts[$0] = {};
                Module.vouts[$0].msgPort = msgChannel.port1;
                w.postMessage({
                    customCmd: 'getVoutMessagePortResult',
                    msgPort: msgChannel.port2,
                }, [msgChannel.port2]);
            }
            else if (msg.customCmd == 'voutMessagePortUnbind') {
                w.removeEventListener('message', workerMessageEvent);
            }
        };

        w.addEventListener('message', workerMessageEvent);
    }, pthread_self(), wnd->handle.canvas);

    getVoutMessagePort(pthread_self());

    EM_ASM({
        Module.vout = {};
        Module.vout.initialized = false;
        Module.vout.dirty = false;
        Module.voutMsgPort.onmessage = function(e){
            let msg = e.data;
            if (msg.customCmd == 'initRenderer')
            {
                _EmscriptenInitRenderer($0);
                Module.vout.initialized = true;
            }
            else if (msg.customCmd == 'renderNext')
            {
                Module.vout.dirty = true;
            }
            else if (msg.customCmd == 'requestChange')
            {
                _EmscriptenRequestChange($0);
            }
            else if (msg.customCmd == 'resize')
            {
                _EmscriptenResize($0, msg['width'], msg['height']);
                Module.offscreenCanvas.width = msg['width'];
                Module.offscreenCanvas.height = msg['height'];
            }
            else if (msg.customCmd == 'close')
            {
                console.log("Cancelling main loop");
                Module.vout.dirty = false;
                _EmscriptenReportDestroy($0);
                _EmscriptenKeepalivePop();
                _emscripten_cancel_main_loop();
                console.log("Main loop cancelled");
            }
            else if (msg.customCmd == 'mousemove')
            {
                _mousemove($1, msg['offsetX'], msg['offsetY']);
            }
            else if (msg.customCmd == 'mousedown')
            {
                _mousedown($1);
            }
            else if (msg.customCmd == 'mouseup')
            {
                _mouseup($1);
            }
        };
    }, gl, wnd);

    sys->context = createGlContext(width, height);

    if (!sys->context)
        return; // TODO

    // Check that the WebGL context is valid
    if (emscripten_webgl_make_context_current(sys->context) != EMSCRIPTEN_RESULT_SUCCESS) {
        emscripten_log(EM_LOG_CONSOLE, "failed to make context current");
        //goto error;
        // TODO
        return;
    }
}

static void* OpenglWorkerRunloop(void *opaque)
{
    auto startup = static_cast<struct emscripten_startup *>(opaque);

    vlc_gl_t *gl = startup->gl;
    auto sys = static_cast<emscripten_gl_sys_t *>(gl->sys);

    sys->worker_id = pthread_self();
    OpenglInitialize(gl, startup->width, startup->height);

    MAIN_THREAD_EM_ASM({
        Module.mouseEventListener = function(ev) {
            let w = Module.vouts[$0];
            switch(ev.type) {
                case "mousemove":
                    w.msgPort.postMessage({
                        customCmd: 'mousemove',
                        offsetX: ev.offsetX,
                        offsetY: ev.offsetY,
                    });
                    break;
                case "mousedown":
                    w.msgPort.postMessage({ customCmd: 'mousedown' });
                    break;
                case "mouseup":
                    w.msgPort.postMessage({ customCmd: 'mouseup' });
                    break;
            }
        };

        window.addEventListener("mousemove", Module.mouseEventListener);
        window.addEventListener("mousedown", Module.mouseEventListener);
        window.addEventListener("mouseup", Module.mouseEventListener);
    }, sys->worker_id);

    msg_Dbg(gl, "emscripten initialized");

    if (sys->context == NULL)
    {
        vlc_sem_post(&startup->wait_sem);
        return NULL;
    }

    //bindRendererInit(gl);
    //msg_Dbg(gl, "bound renderer");
    vlc_sem_post(&startup->wait_sem);

    emscripten_runtime_keepalive_push();
    emscripten_set_main_loop_arg(RunloopRender, gl, -1, false);

    return NULL;
}

static int Open (vlc_gl_t *gl, unsigned width, unsigned height, const struct vlc_gl_cfg *cfg)
{
    (void)width, (void)height;
    (void)cfg;
    msg_Dbg(gl, "Opening emscripten opengl");

    vlc_window_t *wnd = gl->surface;
    if (wnd->type != VLC_WINDOW_TYPE_EMSCRIPTEN_WEBGL)
        return VLC_ENOTSUP;

    emscripten_gl_sys_t *sys;

    gl->sys = sys = new (std::nothrow)emscripten_gl_sys_t {};
    if (!sys)
        return VLC_ENOMEM;
    vlc_sem_init(&sys->sem_destroy, 0);
    
    struct emscripten_startup startup = {
        .gl = gl,
        .width = width,
        .height = height,
    };
    vlc_sem_init(&startup.wait_sem, 0);

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
    gl->api_mode = VLC_GL_ASYNC_MODE;
    if (vlc_clone(&sys->worker, OpenglWorkerRunloop, &startup) != VLC_SUCCESS)
    {
        delete sys;
        gl->api_mode = VLC_GL_SYNC_MODE;
        gl->sys = NULL;
        gl->ops = NULL;
        return VLC_EGENERIC;
    }

    vlc_sem_wait(&startup.wait_sem);

    if (sys->context == NULL)
    {
        delete sys;
        gl->api_mode = VLC_GL_SYNC_MODE;
        gl->sys = NULL;
        gl->ops = NULL;
    }
    //EM_ASM({
    //    let w = Module.PThread.pthreads[$0];
    //}, pthread_self();

    // Implement gl routines:
    msg_Dbg(gl, "Opening emscripten opengl succeed");
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
    set_callback(OpenWindow)

    add_submodule ()
    set_shortname("Emscripten GL")
    set_description(N_("Emscripten extension for OpenGL"))
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("opengl es2", 50)
    set_callback(Open)
    add_shortcut("em_webgl")
vlc_module_end()

