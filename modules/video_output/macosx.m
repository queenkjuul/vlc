/*****************************************************************************
 * macosx.m: MacOS X OpenGL provider
 *****************************************************************************
 * Copyright (C) 2001-2013 VLC authors and VideoLAN
 *
 * Authors: Derk-Jan Hartman <hartman at videolan dot org>
 *          Eric Petit <titer@m0k.org>
 *          Benjamin Pracht <bigben at videolan dot org>
 *          Damien Fouilleul <damienf at videolan dot org>
 *          Pierre d'Herbemont <pdherbemont at videolan dot org>
 *          Felix Paul Kühne <fkuehne at videolan dot org>
 *          David Fuhrmann <david dot fuhrmann at googlemail dot com>
 *          Rémi Denis-Courmont
 *          Juho Vähä-Herttua <juhovh at iki dot fi>
 *          Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
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

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <dlfcn.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_opengl.h>
#include <vlc_dialog.h>
#include "opengl/renderer.h"
#include "opengl/vout_helper.h"

/**
 * Forward declarations
 */
static void Close(vlc_gl_t *gl);

static void *OurGetProcAddress(vlc_gl_t *, const char *);
static int OpenglLock (vlc_gl_t *gl);
static void OpenglUnlock (vlc_gl_t *gl);
static void OpenglSwap (vlc_gl_t *gl);

/**
 * Obj-C protocol declaration that drawable-nsobject should follow
 */
@protocol VLCVideoViewEmbedding <NSObject>
- (void)addVoutSubview:(NSView *)view;
- (void)removeVoutSubview:(NSView *)view;
@end

@interface VLCOpenGLVideoView : NSOpenGLView
{
    vlc_gl_t *gl;
}
- (void)setVoutGl:(vlc_gl_t *)gl;
- (void)render;
@end


typedef struct vout_display_sys_t
{
    VLCOpenGLVideoView *glView;
    id<VLCVideoViewEmbedding> container;

    vlc_gl_t *gl;
    vout_display_opengl_t *vgl;

    picture_t *current;
    bool has_first_frame;

    vout_display_cfg_t cfg;
    vout_display_place_t place;
} vout_display_sys_t;

struct gl_sys
{
    CGLContextObj locked_ctx;
    VLCOpenGLVideoView *glView;
};

static void *OurGetProcAddress(vlc_gl_t *gl, const char *name)
{
    VLC_UNUSED(gl);

    return dlsym(RTLD_DEFAULT, name);
}

static int Open(vlc_gl_t *gl, unsigned width, unsigned height,
                const struct vlc_gl_cfg *gl_cfg)
{

    if (gl->surface->type != VLC_WINDOW_TYPE_NSOBJECT)
        return VLC_EGENERIC;

    vout_display_sys_t *sys = calloc(1, sizeof(*sys));

    if (!sys)
        return VLC_ENOMEM;
    sys->has_first_frame = false;
    sys->current = NULL;
    sys->vgl = NULL;
    sys->gl = NULL;

    @autoreleasepool {
        if (!CGDisplayUsesOpenGLAcceleration (kCGDirectMainDisplay))
            msg_Err (gl, "no OpenGL hardware acceleration found. this can lead to slow output and unexpected results");

        gl->sys = sys;

        /* Get the drawable object */
        id container = (__bridge id)gl->surface->handle.nsobject;
        assert(container != nil);

        /* This will be released in Close(), on
         * main thread, after we are done using it. */
        sys->container = container;

        /* Get our main view*/
        dispatch_sync(dispatch_get_main_queue(), ^{
            sys->glView = [[VLCOpenGLVideoView alloc] init];
        });

        if (!sys->glView) {
            msg_Err(gl, "Initialization of open gl view failed");
            goto error;
        }

        [sys->glView setVoutGl:gl];

        /* We don't wait, that means that we'll have to be careful about releasing
         * container.
         * That's why we'll release on main thread in Close(). */
        if ([container respondsToSelector:@selector(addVoutSubview:)]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [container addVoutSubview: sys->glView];
            });
        }
        else if ([container isKindOfClass:[NSView class]]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                NSView *parentView = container;
                [parentView addSubview:sys->glView];
                [sys->glView setFrame:[parentView bounds]];
            });
        } else {
            msg_Err(gl, "Invalid drawable-nsobject object. drawable-nsobject must either be an NSView or comply to the @protocol VLCVideoViewEmbedding.");
            goto error;
        }

        struct gl_sys *glsys = gl->sys = malloc(sizeof(struct gl_sys));
        if (unlikely(!gl->sys))
            goto error;

        glsys->locked_ctx = NULL;
        glsys->glView = sys->glView;

        static const struct vlc_gl_operations gl_ops =
        {
            .sync_mode.make_current = OpenglLock,
            .sync_mode.release_current = OpenglUnlock,
            .swap = OpenglSwap,
            .get_proc_address = OurGetProcAddress,
        };
        gl->ops = &gl_ops;
        gl->api_type = VLC_OPENGL;

        return VLC_SUCCESS;

    error:
        Close(gl);
        return VLC_EGENERIC;
    }
}

static void Close(vlc_gl_t *gl)
{
    vout_display_sys_t *sys = gl->sys;

    @autoreleasepool {
        [sys->glView setVoutGl:nil];

        var_Destroy(gl, "drawable-nsobject");
        assert(((struct gl_sys *)sys->gl->sys)->locked_ctx == NULL);

        dispatch_async(dispatch_get_main_queue(), ^{
            if ([sys->container respondsToSelector:@selector(removeVoutSubview:)]) {
                /* This will retain sys->glView */
                [sys->container removeVoutSubview:sys->glView];
            }

            /* release on main thread as explained in Open() */
            sys->container = nil;
            [sys->glView removeFromSuperview];
            sys->glView = nil;
            free(sys);
        });
    }
}

/*****************************************************************************
 * vout opengl callbacks
 *****************************************************************************/
static int OpenglLock (vlc_gl_t *gl)
{
    struct gl_sys *sys = gl->sys;
    if (![sys->glView respondsToSelector:@selector(openGLContext)])
        return 1;

    assert(sys->locked_ctx == NULL);

    NSOpenGLContext *context = [sys->glView openGLContext];
    CGLContextObj cglcntx = [context CGLContextObj];

    CGLError err = CGLLockContext (cglcntx);
    if (kCGLNoError == err) {
        sys->locked_ctx = cglcntx;
        [context makeCurrentContext];
        return 0;
    }
    return 1;
}

static void OpenglUnlock (vlc_gl_t *gl)
{
    struct gl_sys *sys = gl->sys;
    CGLUnlockContext (sys->locked_ctx);
    sys->locked_ctx = NULL;
}

static void OpenglSwap (vlc_gl_t *gl)
{
    struct gl_sys *sys = gl->sys;
    [[sys->glView openGLContext] flushBuffer];
}

/*****************************************************************************
 * Our NSView object
 *****************************************************************************/
@implementation VLCOpenGLVideoView

#define VLCAssertMainThread() assert([[NSThread currentThread] isMainThread])

/**
 * Gets called by the Open() method.
 */
- (id)init
{
    VLCAssertMainThread();

    /* Warning - this may be called on non main thread */

    NSOpenGLPixelFormatAttribute attribs[] =
    {
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAAccelerated,
        NSOpenGLPFANoRecovery,
        NSOpenGLPFAColorSize, 24,
        NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADepthSize, 24,
        NSOpenGLPFAAllowOfflineRenderers,
        0
    };

    NSOpenGLPixelFormat *fmt = [[NSOpenGLPixelFormat alloc] initWithAttributes:attribs];

    if (!fmt)
        return nil;

    self = [super initWithFrame:NSMakeRect(0,0,10,10) pixelFormat:fmt];

    if (!self)
        return nil;

    /* enable HiDPI support */
    [self setWantsBestResolutionOpenGLSurface:YES];

    /* request our screen's HDR mode (introduced in OS X 10.11) */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpartial-availability"
    if ([self respondsToSelector:@selector(setWantsExtendedDynamicRangeOpenGLSurface:)]) {
        [self setWantsExtendedDynamicRangeOpenGLSurface:YES];
    }
#pragma clang diagnostic pop

    /* Swap buffers only during the vertical retrace of the monitor.
     http://developer.apple.com/documentation/GraphicsImaging/
     Conceptual/OpenGL/chap5/chapter_5_section_44.html */
    GLint params[] = { 1 };
    CGLSetParameter ([[self openGLContext] CGLContextObj], kCGLCPSwapInterval, params);

    [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                      object:NSApplication.sharedApplication
                                                       queue:nil
                                                  usingBlock:^(NSNotification *notification) {
                                                      [self performSelectorOnMainThread:@selector(reshape)
                                                                             withObject:nil
                                                                          waitUntilDone:NO];
                                                  }];

    [self setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    return self;
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

/**
 * Gets called by the Close and Open methods.
 * (Non main thread).
 */
- (void)setVoutGl:(vlc_gl_t *)aGl
{
    @synchronized(self) {
        gl = aGl;
    }
}

/**
 * Local method that locks the gl context.
 */
- (BOOL)lockgl
{
    VLCAssertMainThread();
    NSOpenGLContext *context = [self openGLContext];
    CGLError err = CGLLockContext ([context CGLContextObj]);
    if (err == kCGLNoError)
        [context makeCurrentContext];
    return err == kCGLNoError;
}

/**
 * Local method that unlocks the gl context.
 */
- (void)unlockgl
{
    VLCAssertMainThread();
    CGLUnlockContext ([[self openGLContext] CGLContextObj]);
}

/**
 * Local method that force a rendering of a frame.
 * This will get called if Cocoa forces us to redraw (via -drawRect).
 *
 * NOTE: must be called in a @synchronized() block with sys->glView or self.
 */
- (void)render
{
    if (!gl) {
        glClear (GL_COLOR_BUFFER_BIT);
        return;
    }

    vlc_gl_ReportRender(gl, 4, 4); // TODO

    /*
    if (sys->has_first_frame)
        vout_display_opengl_Display (sys->vgl);
    else
        glClear (GL_COLOR_BUFFER_BIT);
    */
    vlc_gl_Swap(gl);
}

/**
 * Method called by Cocoa when the view is resized.
 */
- (void)reshape
{
    VLCAssertMainThread();

    /* on HiDPI displays, the point bounds don't equal the actual pixel based bounds */
    NSRect bounds = [self convertRectToBacking:[self bounds]];

    @synchronized(self) {
        if (gl == NULL) return;
        vout_display_sys_t *sys = gl->sys;
        sys->cfg.display.width  = bounds.size.width;
        sys->cfg.display.height = bounds.size.height;
    }
    [super reshape];
}

/**
 * Method called by Cocoa when the view is resized or the location has changed.
 * We just need to make sure we are locking here.
 */
- (void)update
{
    VLCAssertMainThread();
    BOOL success = [self lockgl];
    if (!success)
        return;

    [super update];

    [self unlockgl];
}

/**
 * Method called by Cocoa to force redraw.
 */
- (void)drawRect:(NSRect) rect
{
    VLCAssertMainThread();

    @synchronized(self) {
        BOOL success = [self lockgl];
        if (!success)
            return;

        [self render];
        [[self openGLContext] flushBuffer];
        [self unlockgl];
    }
}

- (void)renewGState
{
    // Comment take from Apple GLEssentials sample code:
    // https://developer.apple.com/library/content/samplecode/GLEssentials
    //
    // OpenGL rendering is not synchronous with other rendering on the OSX.
    // Therefore, call disableScreenUpdatesUntilFlush so the window server
    // doesn't render non-OpenGL content in the window asynchronously from
    // OpenGL content, which could cause flickering.  (non-OpenGL content
    // includes the title bar and drawing done by the app with other APIs)

    // In macOS 10.13 and later, window updates are automatically batched
    // together and this no longer needs to be called (effectively a no-op)
    [[self window] disableScreenUpdatesUntilFlush];

    [super renewGState];
}

- (BOOL)isOpaque
{
    return YES;
}

#pragma mark -
#pragma mark Mouse handling

#warning Missing mouse handling, must be implemented in Vout Window

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (BOOL)mouseDownCanMoveWindow
{
    return YES;
}

@end

/**
 * Module declaration
 */
vlc_module_begin ()
    /* Will be loaded even without interface module. see voutgl.m */
    set_shortname ("Mac OS X")
    set_description (N_("Mac OS X OpenGL video output"))
    set_subcategory (SUBCAT_VIDEO_VOUT)
    set_callback_opengl(Open, 290)
    add_shortcut ("macosx", "vout_macosx")
    add_glopts ()

    add_opengl_submodule_renderer()
vlc_module_end ()
