/**
 * @file display.c
 * @brief OpenGL video output module
 */
/*****************************************************************************
 * Copyright © 2010-2011 Rémi Denis-Courmont
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

#include <stdlib.h>
#include <assert.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_opengl.h>
#include "vout_helper.h"

#include "renderer.h"

/* Plugin callbacks */
static int Open(vout_display_t *vd,
                video_format_t *fmtp, vlc_video_context *context);
static void Close(vout_display_t *vd);

#define GL_TEXT N_("OpenGL extension")
#define GLES2_TEXT N_("OpenGL ES 2 extension")
#define PROVIDER_LONGTEXT N_( \
    "Extension through which to use the Open Graphics Library (OpenGL).")

vlc_module_begin ()
    set_subcategory (SUBCAT_VIDEO_VOUT)
#if defined (USE_OPENGL_ES2)
# define API VLC_OPENGL_ES2
# define MODULE_VARNAME "gles2"
    set_shortname (N_("OpenGL ES2"))
    set_description (N_("OpenGL for Embedded Systems 2 video output"))
    set_callback_display(Open, 265)
    add_shortcut ("opengles2", "gles2")
    add_module("gles2", "opengl es2", "any", GLES2_TEXT, PROVIDER_LONGTEXT)

#else

# define API VLC_OPENGL
# define MODULE_VARNAME "gl"
    set_shortname (N_("OpenGL"))
    set_description (N_("OpenGL video output"))
    set_callback_display(Open, 270)
    add_shortcut ("opengl", "gl")
    add_module("gl", "opengl", "any", GL_TEXT, PROVIDER_LONGTEXT)
#endif
    add_glopts ()

    add_opengl_submodule_renderer()
vlc_module_end ()

typedef struct vout_display_sys_t
{
    vout_display_opengl_t *vgl;
    vlc_gl_t *gl;
    vout_display_place_t place;
    bool place_changed;
    bool is_dirty;

    struct {
        picture_t *picture;
        struct vlc_render_subpicture *subpicture;
        vlc_mutex_t lock;
        unsigned width, height;
    } latch;

    struct {
        PFNGLFLUSHPROC Flush;
    } vt;
    vlc_viewpoint_t viewpoint;

    struct {
        video_format_t *fmt;
        const vlc_fourcc_t *spu_chromas;
        struct vlc_video_context *context;
    } cfg;
} vout_display_sys_t;

/* Display callbacks */
static void PicturePrepare(vout_display_t *, picture_t *, const vlc_render_subpicture *, vlc_tick_t);
static void PictureDisplay (vout_display_t *, picture_t *);
static int Control (vout_display_t *, int);

static int SetViewpoint(vout_display_t *vd, const vlc_viewpoint_t *vp)
{
    vout_display_sys_t *sys = vd->sys;
    int ret = vout_display_opengl_SetViewpoint(sys->vgl, vp);
    if (ret != VLC_SUCCESS)
        return ret;

    sys->viewpoint = *vp;
    return VLC_SUCCESS;
}

struct opengl_format_request {
    vout_display_t *vd;
    const video_format_t *fmt;
    vlc_video_context *vctx;
};

static int RequestFormatUpdate(vlc_gl_t *gl, void *opaque)
{
    (void)gl;
    struct opengl_format_request *request = opaque;
    vout_display_sys_t *sys = request->vd->sys;

    int ret = vout_display_opengl_UpdateFormat(sys->vgl, request->fmt, request->vctx);

    /* Force to recompute the viewport on next picture */
    sys->place_changed = true;

    /* Restore viewpoint */
    int vp_ret = vout_display_opengl_SetViewpoint(sys->vgl, &sys->viewpoint);
    /* The viewpoint previously applied is necessarily valid */
    assert(vp_ret == VLC_SUCCESS);
    (void) vp_ret;

    return ret;
}

static int
UpdateFormat(vout_display_t *vd, const video_format_t *fmt,
             vlc_video_context *vctx)
{
    vout_display_sys_t *sys = vd->sys;
    return vlc_gl_RequestChange(sys->gl, RequestFormatUpdate, &(struct opengl_format_request){
        .vd = vd, .vctx = vctx, .fmt = fmt,
    });
}

static const struct vlc_display_operations ops = {
    .close = Close,
    .prepare = PicturePrepare,
    .display = PictureDisplay,
    .control = Control,
    .set_viewpoint = SetViewpoint,
    .update_format = UpdateFormat,
};

static void PlacePicture(vout_display_t *vd, vout_display_place_t *place,
                         struct vout_display_placement dp)
{
    vout_display_sys_t *sys = vd->sys;

    /* Copy the initial source, sine we might rotate it to fake a rotated
     * display also. */
    video_format_t source;
    video_format_Init(&source, 0);
    video_format_Copy(&source, vd->source);

    video_transform_t transform = (video_transform_t)sys->gl->orientation;
    video_format_TransformBy(&source, transform_Inverse(transform));

    if (ORIENT_IS_SWAP(transform)) {
        unsigned width = dp.width;
        dp.width = dp.height;
        dp.height = width;
    }

    vout_display_PlacePicture(place, &source, &dp);
    place->y = dp.height - (place->y + place->height);

    if (ORIENT_IS_SWAP(transform))
    {
        *place = (vout_display_place_t){
            .x = place->y,
            .y = place->x,
            .width = place->height,
            .height = place->width,
        };
    }
    sys->place_changed = true;

    video_format_Clean(&source);
}


/**
 * Destroys the OpenGL context.
 */
static void Close(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;
    vlc_gl_t *gl = sys->gl;

    // TODO
    //vlc_gl_MakeCurrent (gl);
    //vout_display_opengl_Delete (sys->vgl);
    //vlc_gl_ReleaseCurrent (gl);

    vlc_gl_Delete(gl);

    if (sys->latch.picture)
        picture_Release(sys->latch.picture);
    free (sys);
}

static int RendererInit(vlc_gl_t *gl)
{
    vout_display_t *vd = gl->owner.sys;
    vout_display_sys_t *sys = vd->sys;

    msg_Info(gl, "In init renderer callback %s:%s:%d", __FILE__, __func__, __LINE__);

    sys->vt.Flush = vlc_gl_GetProcAddress(gl, "glFlush");
    if (sys->vt.Flush == NULL)
        return VLC_EGENERIC;

    msg_Info(gl, "In init renderer callback %s:%s:%d", __FILE__, __func__, __LINE__);
    sys->vgl = vout_display_opengl_New (
            sys->cfg.fmt, &sys->cfg.spu_chromas,
            gl, &vd->cfg->viewpoint, sys->cfg.context);
    msg_Info(gl, "In init renderer callback %s:%s:%d", __FILE__, __func__, __LINE__);
    if (sys->vgl == NULL)
        return VLC_EGENERIC;

    msg_Info(gl, "In init renderer callback %s:%s:%d", __FILE__, __func__, __LINE__);
    return VLC_SUCCESS;
}

static void RendererDestroy(vlc_gl_t *gl)
{
    vout_display_t *vd = gl->owner.sys;
    vout_display_sys_t *sys = vd->sys;

    vout_display_opengl_Delete(sys->vgl);
}

static void PictureRender(vlc_gl_t *gl, unsigned width, unsigned height)
{
    vout_display_t *vd = gl->owner.sys;
    vout_display_sys_t *sys = vd->sys;

    vlc_mutex_lock(&sys->latch.lock);
    struct vout_display_placement dp = vd->cfg->display;
    dp.width = width;
    dp.height = height;
    PlacePicture(vd, &sys->place, dp);
    sys->place_changed = true;

    if (sys->latch.picture == NULL)
    {
        vlc_mutex_unlock(&sys->latch.lock);
        return;
    }

    picture_t *pic = sys->latch.picture;
    vlc_render_subpicture *subpicture = sys->latch.subpicture;

    vout_display_opengl_Prepare (sys->vgl, pic, subpicture);
    if (sys->place_changed)
    {
        vout_display_opengl_SetOutputSize(sys->vgl, sys->latch.width,
                sys->latch.height);
        vout_display_opengl_Viewport(sys->vgl, sys->place.x, sys->place.y,
                sys->place.width, sys->place.height);
        sys->place_changed = false;
    }
    vout_display_opengl_Display(sys->vgl);
    sys->vt.Flush();

    if (subpicture != NULL)
        vlc_render_subpicture_Delete(subpicture);
    sys->latch.subpicture = NULL;

    picture_Release(sys->latch.picture);
    sys->latch.picture = NULL;
    sys->is_dirty = true;
    vlc_mutex_unlock(&sys->latch.lock);
}

static void PicturePrepare(vout_display_t *vd, picture_t *pic,
                           const vlc_render_subpicture *subpicture,
                           vlc_tick_t date)
{
    VLC_UNUSED(date);
    vout_display_sys_t *sys = vd->sys;

    vlc_mutex_lock(&sys->latch.lock);
    if (sys->latch.picture != NULL)
        picture_Release(sys->latch.picture);
    sys->latch.picture = picture_Hold(pic);
    if (sys->latch.subpicture != NULL)
        vlc_render_subpicture_Delete(sys->latch.subpicture);
    if (subpicture != NULL)
    {
        sys->latch.subpicture = vlc_render_subpicture_Copy(subpicture);
    }
    else
    {
        sys->latch.subpicture = NULL;
    }
    vlc_mutex_unlock(&sys->latch.lock);

    vlc_gl_RenderNext(sys->gl);
    sys->is_dirty = true;
}

static void PictureDisplay (vout_display_t *vd, picture_t *pic)
{
    vout_display_sys_t *sys = vd->sys;
    VLC_UNUSED(pic);

    /* Present on screen */
    if (sys->is_dirty)
    {
        vlc_gl_Swap(sys->gl);
        sys->is_dirty = false;
    }
}

static int Control (vout_display_t *vd, int query)
{
    vout_display_sys_t *sys = vd->sys;

    switch (query)
    {

      case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
        vlc_mutex_lock(&sys->latch.lock);
        const unsigned w = vd->cfg->display.width;
        const unsigned h = vd->cfg->display.height;
        vlc_mutex_unlock(&sys->latch.lock);

        vlc_gl_Resize (sys->gl, w, h);

        vlc_mutex_lock(&sys->latch.lock);
        sys->latch.width = w;
        sys->latch.height = h;
        vlc_mutex_unlock(&sys->latch.lock);
        // fallthrough
      case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
      case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
      case VOUT_DISPLAY_CHANGE_SOURCE_PLACE:
      {
        struct vout_display_placement dp = vd->cfg->display;

        PlacePicture(vd, &sys->place, dp);
        sys->place_changed = true;
        return VLC_SUCCESS;
      }
      default:
        msg_Err (vd, "Unknown request %d", query);
    }
    return VLC_EGENERIC;
}

/**
 * Allocates a surface and an OpenGL context for video output.
 */
static int Open(vout_display_t *vd,
                video_format_t *fmt, vlc_video_context *context)
{
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    if (fmt->i_chroma == VLC_CODEC_CVPX_P010)
        return VLC_EGENERIC;
#endif
    vout_display_sys_t *sys = malloc (sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    vlc_mutex_init(&sys->latch.lock);
    vd->sys = sys;
    sys->vgl = NULL;
    sys->gl = NULL;
    sys->is_dirty = false;
    sys->latch.picture = NULL;
    sys->latch.subpicture = NULL;
    sys->latch.width = vd->cfg->display.width;
    sys->latch.height = vd->cfg->display.height;

    sys->cfg.fmt = fmt;
    sys->cfg.context = context;

    vlc_window_t *surface = vd->cfg->window;
    char *gl_name = var_InheritString(surface, MODULE_VARNAME);

    /* VDPAU GL interop works only with GLX. Override the "gl" option to force
     * it. */
#ifndef USE_OPENGL_ES2
    if (surface->type == VLC_WINDOW_TYPE_XID)
    {
        switch (vd->source->i_chroma)
        {
            case VLC_CODEC_VDPAU_VIDEO:
            {
                /* Force the option only if it was not previously set */
                if (gl_name == NULL || gl_name[0] == 0
                 || strcmp(gl_name, "any") == 0)
                {
                    free(gl_name);
                    gl_name = strdup("glx");
                }
                break;
            }
            default:
                break;
        }
    }
#endif

    static const struct vlc_gl_callbacks gl_cbs = {
        .init = RendererInit,
        .render = PictureRender,
        .destroy = RendererDestroy,
    };

    vd->sys = sys;
    sys->gl = vlc_gl_Create(vd->cfg, API, gl_name, NULL, &gl_cbs, vd);
    free(gl_name);
    if (sys->gl == NULL)
        goto error;
    vd->sys = sys;

    if (vlc_gl_InitRenderer(sys->gl) || sys->vgl == NULL)
        goto error;

    struct vout_display_placement dp = vd->cfg->display;
    PlacePicture(vd, &sys->place, dp);
    sys->place_changed = true;
    vlc_gl_Resize (sys->gl, vd->cfg->display.width, vd->cfg->display.height);

    /* Initialize video display */
    sys->viewpoint = vd->cfg->viewpoint;

    vd->info.subpicture_chromas = sys->cfg.spu_chromas;
    vd->ops = &ops;
    return VLC_SUCCESS;

error:
    if (sys->gl != NULL)
        vlc_gl_Delete(sys->gl);
    vd->sys = NULL;
    free (sys);
    vd->sys = NULL;
    return VLC_EGENERIC;
}
