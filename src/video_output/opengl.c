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

struct vlc_gl_priv_t
{
    vlc_gl_t gl;

    vlc_mutex_t lock;
    vlc_cond_t client_cond;

    atomic_bool dirty;

    /* Only for synchronous implementations */
    struct {
        unsigned width;
        unsigned height;

        bool running;
        vlc_thread_t thread;
    } sync_mode;
};

static void vlc_gl_ReportInit(vlc_gl_t *gl)
{
    if (gl->owner.cbs != NULL && gl->owner.cbs->init != NULL)
        gl->owner.cbs->init(gl);
}

static void vlc_gl_ReportDestroy(vlc_gl_t *gl)
{
    if (gl->owner.cbs != NULL && gl->owner.cbs->destroy != NULL)
        gl->owner.cbs->destroy(gl);
}

static void* OpenglThread(void *opaque)
{
    vlc_gl_t *gl = opaque;
    struct vlc_gl_priv_t *glpriv = container_of(gl, struct vlc_gl_priv_t, gl);
    vlc_mutex_lock(&glpriv->lock);
    vlc_gl_MakeCurrent(gl);
    vlc_gl_ReportInit(gl);
    while (glpriv->sync_mode.running)
    {
        while (!glpriv->dirty)
        {
            vlc_cond_wait(&glpriv->client_cond, &glpriv->lock);
            if (!glpriv->sync_mode.running)
                break;
        }
        vlc_gl_ReportRender(gl, glpriv->sync_mode.width, glpriv->sync_mode.height);
    }
    vlc_gl_ReportDestroy(gl);
    vlc_mutex_unlock(&glpriv->lock);
    vlc_gl_ReleaseCurrent(gl);
    return NULL;
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
                                  int flags, const struct vlc_gl_cfg *gl_cfg,
                                  const struct vlc_gl_callbacks *cbs,
                                  void *owner)
{
    struct vlc_gl_priv_t *glpriv;
    const char *type;
    if (gl_cfg == NULL)
        gl_cfg = &gl_cfg_default;

    enum vlc_gl_api_type api_type;

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

    glpriv = vlc_custom_create(parent, sizeof (*glpriv), "gl");
    if (unlikely(glpriv == NULL))
        return NULL;

    vlc_gl_t *gl = &glpriv->gl;
    gl->api_type = api_type;
    gl->api_mode = VLC_GL_SYNC_MODE;
    gl->orientation = ORIENT_NORMAL;
    gl->device = NULL;
    gl->owner.cbs = cbs;
    gl->owner.sys = owner;
    vlc_mutex_init(&glpriv->lock);
    vlc_cond_init(&glpriv->client_cond);
    atomic_init(&glpriv->dirty, false);

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
        assert(gl->ops->async_mode.init_renderer != NULL);
        assert(gl->ops->async_mode.render_next != NULL);
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
    struct vlc_gl_priv_t *glpriv;
    const char *type;

    vlc_gl_t *gl = CommonOpenglInit(VLC_OBJECT(wnd), flags, gl_cfg, cbs, owner);
    if (gl == NULL)
        return NULL;
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

    return l;
}

vlc_gl_t *vlc_gl_CreateOffscreen(vlc_object_t *parent,
                                 struct vlc_decoder_device *device,
                                 unsigned width, unsigned height,
                                 unsigned flags, const char *name,
                                 const struct vlc_gl_cfg *gl_cfg,
                                 const struct vlc_gl_callbacks *cbs,
                                 void *owner)
{
    struct vlc_gl_priv_t *glpriv;
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

    glpriv = vlc_custom_create(parent, sizeof (*glpriv), "gl");
    if (unlikely(glpriv == NULL))
        return NULL;

    vlc_sem_init(&glpriv->sem_client, 0);
    atomic_init(&glpriv->dirty, false);

    vlc_gl_t *gl = &glpriv->gl;

    gl->api_type = api_type;
    gl->orientation = ORIENT_NORMAL;

    gl->offscreen_chroma_out = VLC_CODEC_UNKNOWN;
    gl->offscreen_vctx_out = NULL;
    gl->owner.sys = owner;
    gl->owner.cbs = cbs;

    gl->surface = NULL;
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
    if (gl->api_mode == VLC_GL_SYNC_MODE)
    {
        assert(gl->ops->sync_mode.make_current);
        assert(gl->ops->sync_mode.release_current);
    }
    else
    {
        assert(gl->ops->async_mode.init_renderer != NULL);
        assert(gl->ops->async_mode.render_next != NULL);
    }
    assert(gl->ops->swap_offscreen);
    assert(gl->ops->get_proc_address);

    return &glpriv->gl;
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

    atomic_store_explicit(&glpriv->dirty, true, memory_order_release);
    switch(gl->api_mode)
    {
        case VLC_GL_SYNC_MODE: {
            assert(gl->ops->sync_mode.make_current != NULL);
            assert(gl->ops->sync_mode.release_current != NULL);
            // TODO
            gl->ops->sync_mode.make_current(gl);
            gl->owner.cbs->render(gl, width, height);
            gl->ops->sync_mode.release_current(gl);
            break;
        }
        case VLC_GL_ASYNC_MODE: {
            assert(gl->ops->async_mode.render_next != NULL);
            gl->ops->async_mode.render_next(gl);
            vlc_sem_wait(&glpriv->sem_client);
            break;
        }
    }
}

void vlc_gl_ReportRender(vlc_gl_t *gl, unsigned width, unsigned height)
{
    struct vlc_gl_priv_t *glpriv = (struct vlc_gl_priv_t *)gl;

    bool dirty = atomic_exchange_explicit(&glpriv->dirty, false, memory_order_acquire);
    if (!dirty)
        return;

    if (gl->owner.cbs && gl->owner.cbs->render)
        gl->owner.cbs->render(gl, width, height);
    vlc_sem_post(&glpriv->sem_client);
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
