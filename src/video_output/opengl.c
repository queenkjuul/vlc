/*****************************************************************************
 * opengl.c: VLC GL API
 *****************************************************************************
 * Copyright (C) 2011 Rémi Denis-Courmont
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

#include <assert.h>
#include <stdlib.h>

#include <vlc_common.h>
#include <vlc_atomic.h>
#include <vlc_opengl.h>
#include <vlc_codec.h>
#include <vlc_vout_display.h>
#include "libvlc.h"
#include <vlc_modules.h>

static const struct vlc_gl_cfg gl_cfg_default = {
    .need_alpha = false
};

enum vlc_gl_command {
    VLC_GL_COMMAND_NONE,
    VLC_GL_COMMAND_SWAP,
};

struct vlc_gl_priv_t
{
    vlc_gl_t gl;

    vlc_mutex_t lock;
    vlc_cond_t client_cond;
    vlc_cond_t sync_cond;

    bool renderer_available;
    bool renderer_initialized;

    struct {
        vlc_gl_change_request cb;
        void *opaque;
        int result;
    } change_request;

    /* Only for synchronous implementations */
    struct {
        unsigned width;
        unsigned height;
        bool dirty;
        enum vlc_gl_command command;

        bool running;
        vlc_thread_t thread;
    } sync_mode;
};

static int ReportInit(vlc_gl_t *gl)
{
    struct vlc_gl_priv_t *glpriv = container_of(gl, struct vlc_gl_priv_t, gl);
    vlc_mutex_assert(&glpriv->lock);

    int ret = VLC_EGENERIC;
    if (gl->owner.cbs != NULL && gl->owner.cbs->init != NULL)
    {
        fprintf(stderr, "INITIALIZING RENDERER %s:%d\n", __func__, __LINE__);
        ret = gl->owner.cbs->init(gl);
        fprintf(stderr, "INITIALIZING RENDERER %s:%d\n", __func__, __LINE__);
    }

    if (ret == VLC_SUCCESS)
    {
        glpriv->renderer_initialized = true;
        vlc_cond_signal(&glpriv->sync_cond);
    }
        fprintf(stderr, "INITIALIZING RENDERER %s:%d\n", __func__, __LINE__);
    return ret;
}

int vlc_gl_ReportInit(vlc_gl_t *gl)
{
    struct vlc_gl_priv_t *glpriv = container_of(gl, struct vlc_gl_priv_t, gl);
    vlc_mutex_lock(&glpriv->lock);
    int ret = ReportInit(gl);
    vlc_mutex_unlock(&glpriv->lock);
    return ret;
}

static bool ReportRender(vlc_gl_t *gl, unsigned width, unsigned height)
{
    if (gl->owner.cbs && gl->owner.cbs->render)
        return gl->owner.cbs->render(gl, width, height);
    return false;
}

bool vlc_gl_ReportRender(vlc_gl_t *gl, unsigned width, unsigned height)
{
    struct vlc_gl_priv_t *glpriv = (struct vlc_gl_priv_t *)gl;
    vlc_mutex_lock(&glpriv->lock);
    const bool ret = ReportRender(gl, width, height);
    vlc_mutex_unlock(&glpriv->lock);
    return ret;
}

static void ReportDestroy(vlc_gl_t *gl)
{
    if (gl->owner.cbs != NULL && gl->owner.cbs->destroy != NULL)
        gl->owner.cbs->destroy(gl);
}


void vlc_gl_ReportDestroy(vlc_gl_t *gl)
{
    struct vlc_gl_priv_t *glpriv = (struct vlc_gl_priv_t *)gl;
    vlc_mutex_lock(&glpriv->lock);
    ReportDestroy(gl);
    vlc_mutex_unlock(&glpriv->lock);
}

int vlc_gl_RequestInit(vlc_gl_t *gl)
{
    struct vlc_gl_priv_t *glpriv = container_of(gl, struct vlc_gl_priv_t, gl);
    vlc_mutex_lock(&glpriv->lock);
    if (glpriv->renderer_initialized) 
    {
        vlc_mutex_unlock(&glpriv->lock);
        return VLC_EGENERIC;
    }

    glpriv->renderer_available = true;
    if (gl->api_mode == VLC_GL_ASYNC_MODE)
    {
        assert(gl->ops->async_mode.init_renderer != NULL);
        int ret = gl->ops->async_mode.init_renderer(gl);
        if (ret != VLC_SUCCESS)
        {
            glpriv->renderer_available = false;
            vlc_mutex_unlock(&glpriv->lock);
            return VLC_EGENERIC;
        }

        while (glpriv->renderer_available && !glpriv->renderer_initialized)
            vlc_cond_wait(&glpriv->sync_cond, &glpriv->lock);
    }
    else
    {
        assert(gl->api_mode == VLC_GL_SYNC_MODE);
        if (gl->owner.cbs == NULL || gl->owner.cbs->init == NULL)
        {
            glpriv->renderer_available = false;
            vlc_mutex_unlock(&glpriv->lock);
            return VLC_ENOTSUP;
        }
        vlc_cond_signal(&glpriv->client_cond);

        while (glpriv->renderer_available && !glpriv->renderer_initialized)
            vlc_cond_wait(&glpriv->sync_cond, &glpriv->lock);
    }

    int ret = glpriv->renderer_initialized ? VLC_SUCCESS : VLC_EGENERIC;
    vlc_mutex_unlock(&glpriv->lock);
    return ret;
}

int vlc_gl_RequestChange(vlc_gl_t *gl, vlc_gl_change_request change_cb, void *opaque)
{
    struct vlc_gl_priv_t *glpriv = container_of(gl, struct vlc_gl_priv_t, gl);
    vlc_mutex_lock(&glpriv->lock);
    glpriv->change_request.cb = change_cb;
    glpriv->change_request.opaque = opaque;

    if (gl->api_mode == VLC_GL_SYNC_MODE)
        vlc_cond_signal(&glpriv->client_cond);
    else
    {
        assert(gl->ops->async_mode.request_change != NULL);
        gl->ops->async_mode.request_change(gl);
    }

    while (glpriv->change_request.cb != NULL)
        vlc_cond_wait(&glpriv->sync_cond, &glpriv->lock);
    vlc_mutex_unlock(&glpriv->lock);
    return VLC_SUCCESS;
}

void vlc_gl_ApplyChanges(vlc_gl_t *gl)
{
    struct vlc_gl_priv_t *glpriv = container_of(gl, struct vlc_gl_priv_t, gl);
    assert(glpriv->change_request.cb != NULL);

    vlc_mutex_lock(&glpriv->lock);
    glpriv->change_request.result = glpriv->change_request.cb(gl, glpriv->change_request.opaque);
    glpriv->change_request.cb = NULL;
    vlc_cond_signal(&glpriv->sync_cond);
    vlc_mutex_unlock(&glpriv->lock);
}

static void* OpenglThread(void *opaque)
{
    vlc_gl_t *gl = opaque;
    struct vlc_gl_priv_t *glpriv = container_of(gl, struct vlc_gl_priv_t, gl);
    vlc_mutex_lock(&glpriv->lock);
    vlc_gl_MakeCurrent(gl);

    do {
        while (!glpriv->renderer_available && glpriv->sync_mode.running)
            vlc_cond_wait(&glpriv->client_cond, &glpriv->lock);

        if (glpriv->renderer_available && ReportInit(gl) != VLC_SUCCESS)
            glpriv->renderer_available = false;
    } while (!glpriv->renderer_initialized && glpriv->sync_mode.running);

    glpriv->renderer_initialized = true;
    vlc_cond_signal(&glpriv->sync_cond);

    while (glpriv->sync_mode.running)
    {
        while (glpriv->sync_mode.dirty == false &&
               glpriv->sync_mode.command == VLC_GL_COMMAND_NONE)
        {
            vlc_cond_wait(&glpriv->client_cond, &glpriv->lock);
            if (!glpriv->sync_mode.running)
                break;
        }

        if (glpriv->sync_mode.dirty)
        {
            ReportRender(gl, glpriv->sync_mode.width, glpriv->sync_mode.height);
            glpriv->sync_mode.dirty = false;
            vlc_cond_signal(&glpriv->sync_cond);
        }

        if (glpriv->sync_mode.command == VLC_GL_COMMAND_SWAP)
        {
            /* HACK: some current opengl implementation are relying on
             *       the context being removed from the thread before swapping
             *       and don't display anything without this. */
            vlc_gl_ReleaseCurrent(gl);
            gl->ops->swap(gl);
            vlc_gl_MakeCurrent(gl);
            glpriv->sync_mode.command = VLC_GL_COMMAND_NONE;
            vlc_cond_signal(&glpriv->sync_cond);
        }
    }
    ReportDestroy(gl);
    vlc_mutex_unlock(&glpriv->lock);
    vlc_gl_ReleaseCurrent(gl);
    return NULL;
}


void vlc_gl_Resize(vlc_gl_t *gl, unsigned w, unsigned h)
{
    struct vlc_gl_priv_t *glpriv = container_of(gl, struct vlc_gl_priv_t, gl);
    vlc_mutex_lock(&glpriv->lock);
    if (gl->ops->resize != NULL)
        gl->ops->resize(gl, w, h);
    glpriv->sync_mode.width = w;
    glpriv->sync_mode.height = h;
    vlc_mutex_unlock(&glpriv->lock);
}


void vlc_gl_Swap(vlc_gl_t *gl)
{
    struct vlc_gl_priv_t *glpriv = container_of(gl, struct vlc_gl_priv_t, gl);
    vlc_mutex_lock(&glpriv->lock);

    if (gl->api_mode == VLC_GL_SYNC_MODE)
    {
        assert(glpriv->sync_mode.command == VLC_GL_COMMAND_NONE);
        glpriv->sync_mode.command = VLC_GL_COMMAND_SWAP;
        vlc_cond_signal(&glpriv->client_cond);
        while (glpriv->sync_mode.command != VLC_GL_COMMAND_NONE)
            vlc_cond_wait(&glpriv->sync_cond, &glpriv->lock);
    }
    else
    {
        assert(gl->api_mode == VLC_GL_ASYNC_MODE);
        gl->ops->swap(gl);
    }

    vlc_mutex_unlock(&glpriv->lock);
}

static int vlc_gl_start(void *func, bool forced, va_list ap)
{
    vlc_gl_activate activate = func;
    vlc_gl_t *gl = va_arg(ap, vlc_gl_t *);
    unsigned width = va_arg(ap, unsigned);
    unsigned height = va_arg(ap, unsigned);
    const struct vlc_gl_cfg *gl_cfg = va_arg(ap, const struct vlc_gl_cfg *);

    int ret = activate(gl, width, height, gl_cfg);
    if (ret)
        vlc_objres_clear(VLC_OBJECT(gl));
    (void) forced;
    return ret;
}

static vlc_gl_t* CommonOpenglInit(vlc_object_t *parent,
                                  unsigned width, unsigned height,
                                  const struct vlc_gl_callbacks *cbs,
                                  void *owner)
{
    struct vlc_gl_priv_t *glpriv;

    glpriv = vlc_custom_create(parent, sizeof (*glpriv), "gl");
    if (unlikely(glpriv == NULL))
        return NULL;

    glpriv->sync_mode.dirty = false;
    glpriv->sync_mode.width = width;
    glpriv->sync_mode.height = height;
    glpriv->sync_mode.command = VLC_GL_COMMAND_NONE;
    glpriv->renderer_available = false;
    vlc_mutex_init(&glpriv->lock);
    vlc_cond_init(&glpriv->client_cond);
    vlc_cond_init(&glpriv->sync_cond);

    vlc_gl_t *gl = &glpriv->gl;
    gl->surface = NULL;
    gl->api_mode = VLC_GL_SYNC_MODE;
    gl->orientation = ORIENT_NORMAL;
    gl->device = NULL;
    gl->owner.cbs = cbs;
    gl->owner.sys = owner;

    return gl;
}

static vlc_gl_t* CommonOpenglSetup(vlc_gl_t *gl)
{
    struct vlc_gl_priv_t *glpriv = container_of(gl, struct vlc_gl_priv_t, gl);
    assert(gl->ops);

    if (gl->api_mode == VLC_GL_SYNC_MODE)
    {
        assert(gl->ops->sync_mode.make_current != NULL);
        assert(gl->ops->sync_mode.release_current != NULL);
        glpriv->sync_mode.running = true;
        if (vlc_clone(&glpriv->sync_mode.thread, OpenglThread, gl) != VLC_SUCCESS)
        {
            if (gl->ops->close != NULL)
                gl->ops->close(gl);
            vlc_object_delete(gl);
            return NULL;
        }
    }
    else
    {
        //assert(gl->ops->async_mode.init_renderer != NULL);
        assert(gl->ops->async_mode.render_next != NULL);
        assert(gl->ops->async_mode.request_change != NULL);
    }
    assert(gl->ops->get_proc_address);

    return gl;
}

vlc_gl_t *vlc_gl_Create(const struct vout_display_cfg *restrict cfg,
                        unsigned flags, const char *name,
                        const struct vlc_gl_cfg * gl_cfg,
                        const struct vlc_gl_callbacks *cbs, void *owner)
{
    vlc_window_t *wnd = cfg->window;
    const char *type;
    enum vlc_gl_api_type api_type;
    if (gl_cfg == NULL)
        gl_cfg = &gl_cfg_default;

    switch (flags /*& VLC_OPENGL_API_MASK*/)
    {
        case VLC_OPENGL:
            type = "opengl";
            api_type = VLC_OPENGL;
            break;
        case VLC_OPENGL_ES2:
            type = "opengl es2";
            api_type = VLC_OPENGL_ES2;
            break;
        default:
            return NULL;
    }
    vlc_gl_t *gl = CommonOpenglInit(VLC_OBJECT(wnd), 
                                    cfg->display.width, cfg->display.height,
                                    cbs, owner);
    if (gl == NULL)
        return NULL;
    gl->api_type = api_type;
    gl->surface = wnd;
    gl->module = vlc_module_load(vlc_object_logger(gl), type, name, true,
                                 vlc_gl_start, gl,
                                 cfg->display.width, cfg->display.height, gl_cfg);
    if (gl->module == NULL)
    {
        vlc_object_delete(gl);
        return NULL;
    }
    assert(gl->ops);
    assert(gl->ops->swap);

    return CommonOpenglSetup(gl);
}

vlc_gl_t *vlc_gl_CreateOffscreen(vlc_object_t *parent,
                                 struct vlc_decoder_device *device,
                                 unsigned width, unsigned height,
                                 unsigned flags, const char *name,
                                 const struct vlc_gl_cfg *gl_cfg,
                                 const struct vlc_gl_callbacks *cbs,
                                 void *owner)
{
    const char *type;

    enum vlc_gl_api_type api_type;
    if (gl_cfg == NULL)
        gl_cfg = &gl_cfg_default;

    switch (flags /*& VLC_OPENGL_API_MASK*/)
    {
        case VLC_OPENGL:
            type = "opengl offscreen";
            api_type = VLC_OPENGL;
            break;
        case VLC_OPENGL_ES2:
            type = "opengl es2 offscreen";
            api_type = VLC_OPENGL_ES2;
            break;
        default:
            return NULL;
    }

    vlc_gl_t *gl = CommonOpenglInit(parent, width, height, cbs, owner);
    if (unlikely(gl == NULL))
        return NULL;

    gl->api_type = api_type;
    gl->offscreen_chroma_out = VLC_CODEC_UNKNOWN;
    gl->offscreen_vctx_out = NULL;
    gl->device = device ? vlc_decoder_device_Hold(device) : NULL;
    gl->module = vlc_module_load(vlc_object_logger(gl), type, name, true,
                                 vlc_gl_start, gl, width, height, gl_cfg);
    if (gl->module == NULL)
    {
        vlc_object_delete(gl);
        return NULL;
    }

    /* The implementation must initialize the output chroma */
    assert(gl->offscreen_chroma_out != VLC_CODEC_UNKNOWN);
    assert(gl->ops);
    assert(gl->ops->swap_offscreen);

    return CommonOpenglSetup(gl);
}

void vlc_gl_Delete(vlc_gl_t *gl)
{
    struct vlc_gl_priv_t *glpriv = container_of(gl, struct vlc_gl_priv_t, gl);
    if (gl->api_mode == VLC_GL_SYNC_MODE)
    {
        vlc_mutex_lock(&glpriv->lock);
        glpriv->sync_mode.running = false;
        vlc_cond_signal(&glpriv->client_cond);
        vlc_mutex_unlock(&glpriv->lock);
        vlc_join(glpriv->sync_mode.thread, NULL);
    }

    if (gl->ops->close != NULL)
        gl->ops->close(gl);

    if (gl->device)
        vlc_decoder_device_Release(gl->device);

    vlc_objres_clear(VLC_OBJECT(gl));
    vlc_object_delete(gl);
}

#include <vlc_window.h>

void vlc_gl_RenderNext(vlc_gl_t *gl)
{
    struct vlc_gl_priv_t *glpriv = (struct vlc_gl_priv_t *)gl;

    switch(gl->api_mode)
    {
        case VLC_GL_SYNC_MODE: {
            assert(gl->ops->sync_mode.make_current != NULL);
            assert(gl->ops->sync_mode.release_current != NULL);
            vlc_mutex_lock(&glpriv->lock);
            glpriv->sync_mode.dirty = true;
            vlc_cond_signal(&glpriv->client_cond);
            while (glpriv->sync_mode.dirty)
                vlc_cond_wait(&glpriv->sync_cond, &glpriv->lock);
            vlc_mutex_unlock(&glpriv->lock);
            break;
        }
        case VLC_GL_ASYNC_MODE: {
            assert(gl->ops->async_mode.render_next != NULL);
            gl->ops->async_mode.render_next(gl);
            break;
        }
    }
}

typedef struct vlc_gl_surface
{
    int width;
    int height;
    vlc_mutex_t lock;
} vlc_gl_surface_t;

static void vlc_gl_surface_ResizeNotify(vlc_window_t *surface,
                                        unsigned width, unsigned height,
                                        vlc_window_ack_cb cb, void *opaque)
{
    vlc_gl_surface_t *sys = surface->owner.sys;

    msg_Dbg(surface, "resized to %ux%u", width, height);

    vlc_mutex_lock(&sys->lock);
    sys->width = width;
    sys->height = height;

    if (cb != NULL)
        cb(surface, width, height, opaque);
    vlc_mutex_unlock(&sys->lock);
}

vlc_gl_t *vlc_gl_surface_Create(vlc_object_t *obj,
                                const vlc_window_cfg_t *cfg,
                                struct vlc_window **restrict wp,
                                const struct vlc_gl_cfg *gl_cfg,
                                const struct vlc_gl_callbacks *gl_cbs, void *gl_owner)
{
    vlc_gl_surface_t *sys = malloc(sizeof (*sys));
    if (unlikely(sys == NULL))
        return NULL;

    sys->width = cfg->width;
    sys->height = cfg->height;
    vlc_mutex_init(&sys->lock);

    static const struct vlc_window_callbacks cbs = {
        .resized = vlc_gl_surface_ResizeNotify,
    };
    vlc_window_owner_t owner = {
        .cbs = &cbs,
        .sys = sys,
    };
    char *modlist = var_InheritString(obj, "window");

    vlc_window_t *surface = vlc_window_New(obj, modlist, &owner, cfg);
    free(modlist);
    if (surface == NULL)
        goto error;
    if (vlc_window_Enable(surface)) {
        vlc_window_Delete(surface);
        goto error;
    }
    if (wp != NULL)
        *wp = surface;

    /* TODO: support ES? */
    struct vout_display_cfg dcfg = {
        .window = surface,
        .display = { .width = cfg->width, cfg->height },
    };

    vlc_mutex_lock(&sys->lock);
    if (sys->width >= 0 && sys->height >= 0) {
        dcfg.display.width = sys->width;
        dcfg.display.height = sys->height;
        sys->width = -1;
        sys->height = -1;
    }
    vlc_mutex_unlock(&sys->lock);

    vlc_gl_t *gl = vlc_gl_Create(&dcfg, VLC_OPENGL, NULL, gl_cfg, gl_cbs, gl_owner);
    if (gl == NULL) {
        vlc_window_Disable(surface);
        vlc_window_Delete(surface);
        goto error;
    }

    return gl;

error:
    free(sys);
    return NULL;
}

/**
 * Checks if the dimensions of the surface used by the OpenGL context have
 * changed (since the previous call), and  the OpenGL viewport should be
 * updated.
 * \return true if at least one dimension has changed, false otherwise
 * \warning This function is intrinsically race-prone.
 * The dimensions can change asynchronously.
 */
bool vlc_gl_surface_CheckSize(vlc_gl_t *gl, unsigned *restrict width,
                              unsigned *restrict height)
{
    vlc_window_t *surface = gl->surface;
    vlc_gl_surface_t *sys = surface->owner.sys;
    bool ret = false;

    vlc_mutex_lock(&sys->lock);
    if (sys->width >= 0 && sys->height >= 0)
    {
        *width = sys->width;
        *height = sys->height;
        sys->width = -1;
        sys->height = -1;

        vlc_gl_Resize(gl, *width, *height);
        ret = true;
    }
    vlc_mutex_unlock(&sys->lock);
    return ret;
}

void vlc_gl_surface_Destroy(vlc_gl_t *gl)
{
    vlc_window_t *surface = gl->surface;
    vlc_gl_surface_t *sys = surface->owner.sys;

    vlc_gl_Delete(gl);
    vlc_window_Disable(surface);
    vlc_window_Delete(surface);
    free(sys);
}
