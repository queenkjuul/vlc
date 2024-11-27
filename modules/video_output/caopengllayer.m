/*****************************************************************************
 * caopengllayer.m: CAOpenGLLayer (Mac OS X) video output
 *****************************************************************************
 * Copyright (C) 2014-2017 VLC authors and VideoLAN
 *
 * Authors: David Fuhrmann <david dot fuhrmann at googlemail dot com>
 *          Felix Paul Kühne <fkuehne at videolan dot org>
 *          Pierre d'Herbemont <pdherbemont at videolan dot org>
 *
 * Some of the code is based on mpv's video_layer.swift by "der richter"
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

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_opengl.h>
#include <vlc_atomic.h>

#import <QuartzCore/QuartzCore.h>
#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <dlfcn.h>

#include "opengl/renderer.h"
#include "opengl/vout_helper.h"

/**
 * Protocol declaration that drawable-nsobject should follow
 */
@protocol VLCOpenGLVideoViewEmbedding <NSObject>
- (void)addVoutSubview:(NSView *)view;
- (void)removeVoutSubview:(NSView *)view;
@end

/**
 * Layer subclass that handles OpenGL video rendering
 */
@interface VLCCAOpenGLLayer : CAOpenGLLayer
{
    NSLock *_displayLock;
    vlc_gl_t *_gl; // All accesses to this must be @synchronized(self)
                   // unless you can be sure it won't be called in teardown
    CGLContextObj _glContext;
    atomic_bool _is_ready;
}

- (instancetype)init:(vlc_gl_t *)gl context:(CGLContextObj)context;
- (void)displayFromVout;
- (void)vlcClose;
- (void)markReady;
@end

/**
 * View subclass which is backed by a VLCCAOpenGLLayer
 */
#if __MAC_OS_X_VERSION_MAX_ALLOWED < 101400
// macOS SDKs lower than 10.14 did not have a NSViewLayerContentScaleDelegate
// protocol definition, but its not needed, it will work fine without it as the
// delegate method even existed before, just not the protocol.
@interface VLCVideoLayerView : NSView <CALayerDelegate>
#else
@interface VLCVideoLayerView : NSView <CALayerDelegate, NSViewLayerContentScaleDelegate>
#endif
{
    vlc_gl_t *_gl; // All accesses to this must be @synchronized(self)
    id _container;

    CGLContextObj _context; // The CGL context managed by us
    CGLContextObj _context_previous; // The previously current CGL context, if any
}

- (instancetype)init:(vlc_gl_t *)gl;
- (void)vlcClose;

- (int)lockContext;
- (void)unlockContext;
- (void)swap;
@end

#pragma mark -
#pragma mark OpenGL context helpers

// kCGLRenderer* enum value define card value, but family is enough here.
// However they follow some pattern by familly.
#define kRendererIntelFamilyMask 0x00024000

/*
 * GL API does not provide a way to know if a device is a lowpower one.
 * We could make some guess here:
 * On a MacBookPro (intel):
 *   - GeForce and Radeon card could be discrete or external.
 *   - Intel could be integrated or external.
 * To be check MacPro or MacMini or ARM ones.
 */
static bool vlc_IsLowPowerDevice(GLint renderer_id) {
    int renderer_vendor = renderer_id & kCGLRendererIDMatchingMask;
    // Consider Intel familly card as low power devices.
    return renderer_vendor == kCGLRendererIntel900ID ||
           renderer_vendor == kCGLRendererIntelX3100ID ||
           renderer_vendor == kCGLRendererIntelHDID ||
           renderer_vendor == kCGLRendererIntelHD4000ID ||
           renderer_vendor == kCGLRendererIntelHD5000ID;
}

/*
 * Search for a low power device.
 * Without proper API (like Metal), here we try to look on all the available
 * renderer and filter them.
 * We are looking for the one attached to the main display, accelerated, and
 * with low power consumption.
 * Without any match, we let CGLChoosePixelFormat/CGLCreateContext do as before.
 */
static GLint vlc_SearchGLRendererId() {
    CGLRendererInfoObj renderer_info = NULL;
    GLint renderer_count = 0;
    if (CGLQueryRendererInfo((GLuint)-1, &renderer_info, &renderer_count) !=
        kCGLNoError)
      return -1;

    GLint best_match = -1;
    for (GLint i = 0; i < renderer_count && best_match == -1; ++i) {
      GLint renderer_id = -1;
      if (CGLDescribeRenderer(renderer_info, i, kCGLRPRendererID,
                              &renderer_id) != kCGLNoError)
        break;
      GLint accelerated = 0;
      if (CGLDescribeRenderer(renderer_info, i, kCGLRPAccelerated,
                              &accelerated) != kCGLNoError)
        break;
      if (!accelerated)
        continue; // avoid not accelerated device
      GLint display = -1;
      if (CGLDescribeRenderer(renderer_info, i, kCGLRPDisplayMask, &display) !=
          kCGLNoError)
        break;
      CGDirectDisplayID display_id = CGOpenGLDisplayMaskToDisplayID(display);
      if (display_id != CGMainDisplayID())
        continue;
      if (vlc_IsLowPowerDevice(renderer_id))
        best_match = renderer_id;
    }
    CGLDestroyRendererInfo(renderer_info);
    return best_match;
}

/**
 * Create a new CGLContextObj for use by VLC
 * This function may try various pixel formats until it finds a suitable/compatible
 * one that works on the given hardware.
 * \return CGLContextObj or NULL in case of error
 */
static CGLContextObj vlc_CreateCGLContext(void)
{
    CGLError err;
    GLint npix = 0;
    CGLPixelFormatObj pix;
    CGLContextObj ctx;

    GLint renderer_id = vlc_SearchGLRendererId();

    CGLPixelFormatAttribute attribs[15] = {
        kCGLPFAAllRenderers,
        kCGLPFAAllowOfflineRenderers,
        kCGLPFADoubleBuffer,
        kCGLPFAAccelerated,
        kCGLPFANoRecovery,
        kCGLPFAColorSize, 24,
        kCGLPFAAlphaSize, 8,
        kCGLPFADepthSize, 24,

        // Enable automatic graphics switching support, important on Macs
        // with dedicated GPUs, as it allows to not always use the dedicated
        // GPU which has more power consumption
        kCGLPFASupportsAutomaticGraphicsSwitching,
        0
    };

    // A low power renderer was found, ask to use it.
    if (renderer_id != -1) {
        attribs[12] = kCGLPFARendererID;
        attribs[13] = renderer_id;
        attribs[14] = 0;
    }
    err = CGLChoosePixelFormat(attribs, &pix, &npix);
    if (err != kCGLNoError || pix == NULL) {
        return NULL;
    }

    err = CGLCreateContext(pix, NULL, &ctx);
    if (err != kCGLNoError || ctx == NULL) {
        return NULL;
    }

    CGLDestroyPixelFormat(pix);
    return ctx;
}

/**
 * Flush the OpenGL context
 * In case of double-buffering swaps the back buffer with the front buffer.
 * \note This function implicitly calls \c glFlush() before it returns.
 */
static void gl_cb_Swap(vlc_gl_t *vlc_gl)
{
    VLCVideoLayerView *view = (__bridge VLCVideoLayerView *)vlc_gl->sys;
    VLCCAOpenGLLayer *layer = (VLCCAOpenGLLayer *)[view layer];
    [layer markReady];
    [view swap];
}

/**
 * Make the OpenGL context the current one
 * Makes the CGL context the current context, if it is not already the current one,
 * and locks it.
 */
static int gl_cb_MakeCurrent(vlc_gl_t *vlc_gl)
{
    VLCVideoLayerView *view = (__bridge VLCVideoLayerView *)vlc_gl->sys;
    return [view lockContext];
}

/**
 * Make the OpenGL context no longer current one.
 * Makes the previous context the current one and unlocks the CGL context.
 */
static void gl_cb_ReleaseCurrent(vlc_gl_t *vlc_gl)
{
    VLCVideoLayerView *view = (__bridge VLCVideoLayerView *)vlc_gl->sys;
    [view unlockContext];
}

/**
 * Look up OpenGL symbols by name
 */
static void *gl_cb_GetProcAddress(vlc_gl_t *vlc_gl, const char *name)
{
    VLC_UNUSED(vlc_gl);

    return dlsym(RTLD_DEFAULT, name);
}

static void gl_cb_RenderNext(vlc_gl_t *vlc_gl)
{
    VLCVideoLayerView *view = (__bridge VLCVideoLayerView *)vlc_gl->sys;
    VLCCAOpenGLLayer *layer = (VLCCAOpenGLLayer *)[view layer];
    [layer markReady];
    [layer displayFromVout];
}

static int gl_cb_InitRenderer(vlc_gl_t *gl)
{
    VLCVideoLayerView *view = (__bridge VLCVideoLayerView *)gl->sys;
    dispatch_sync(dispatch_get_main_queue(), ^{
        [view lockContext];
        gl->owner.cbs->init(gl);
        [view unlockContext];
    });

    return VLC_SUCCESS;
}

static void gl_cb_RequestChange(vlc_gl_t *gl)
{
    VLCVideoLayerView *view = (__bridge VLCVideoLayerView *)gl->sys;
    dispatch_async(dispatch_get_main_queue(), ^{
        [view lockContext];
        vlc_gl_ApplyChanges(gl);
        [view unlockContext];
    });
}


static void CloseOpenGL(vlc_gl_t *gl)
{
    VLCVideoLayerView *view = (__bridge_transfer VLCVideoLayerView *)gl->sys;
    [view vlcClose];
    view = nil;
}

static int OpenOpenGL(vlc_gl_t *gl, unsigned width, unsigned height,
                      const struct vlc_gl_cfg *cfg)
{
    id container = (__bridge id)gl->surface->handle.nsobject;
    if (!container) {
        msg_Err(gl, "No drawable-nsobject found!");
        return VLC_ENOTSUP;
    }

    dispatch_sync(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            VLCVideoLayerView *videoView = [[VLCVideoLayerView alloc] init:gl];
            if (videoView == nil)
                return;
            gl->sys = (__bridge_retained void*)videoView;
        }
    });

    if (unlikely(gl->sys == NULL))
        return VLC_ENOMEM;

    static const struct vlc_gl_operations gl_ops =
    {
        .async_mode.init_renderer = gl_cb_InitRenderer,                        
        .async_mode.request_change = gl_cb_RequestChange,
        .async_mode.render_next = gl_cb_RenderNext,
        .swap = gl_cb_Swap,
        .get_proc_address = gl_cb_GetProcAddress,
        .close = CloseOpenGL,
    };
    gl->ops = &gl_ops;
    gl->api_mode = VLC_GL_ASYNC_MODE;
    gl->api_type = VLC_OPENGL;

    return VLC_SUCCESS;
}

#pragma mark -
#pragma mark VLCVideoLayerView

@implementation VLCVideoLayerView

- (instancetype)init:(vlc_gl_t *)gl
{
    self = [super init];
    if (self == nil)
        return nil;
    _gl = gl;

    _context = vlc_CreateCGLContext();
    if (_context == NULL) {
        msg_Err(_gl, "Failure to create CGL context!");
        return nil;
    }

    _container = (__bridge id)gl->surface->handle.nsobject;
    assert(_container != nil);

    // Add video view to container
    if ([_container respondsToSelector:@selector(addVoutSubview:)]) {
        [_container addVoutSubview:self];
    } else if ([_container isKindOfClass:[NSView class]]) {
        NSView *containerView = _container;
        [containerView addSubview:self];
        [self setFrame:containerView.bounds];
    } else {
        CGLReleaseContext(_context);
        _context = NULL;
        return nil;
    }

    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.wantsLayer = YES;
    return self;
}

- (int)lockContext {
    _context_previous = CGLGetCurrentContext();

    CGLError err;
    if (_context_previous != _context) {
        err = CGLSetCurrentContext(_context);
        if (err != kCGLNoError) {
            msg_Err(_gl, "Failure setting current CGLContext: %s", CGLErrorString(err));
            return VLC_EGENERIC;
        }
    }

    err = CGLLockContext(_context);
    if (err != kCGLNoError) {
        msg_Err(_gl, "Failure locking CGLContext: %s", CGLErrorString(err));
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

- (void)unlockContext {
    CGLError err;

    assert(CGLGetCurrentContext() == _context);

    err = CGLUnlockContext(_context);
    if (err != kCGLNoError) {
        msg_Err(_gl, "Failure unlocking CGLContext: %s", CGLErrorString(err));
        abort();
    }

    if (_context_previous != _context) {
        err = CGLSetCurrentContext(_context_previous);
        if (err != kCGLNoError) {
            msg_Err(_gl, "Failure restoring previous CGLContext: %s", CGLErrorString(err));
            abort();
        }
    }

    _context_previous = NULL;
}

- (void)swap {
    // Copies a double-buffered contexts back buffer to front buffer, calling
    // glFlush before this is not needed and discouraged for performance reasons.
    // An implicit glFlush happens before CGLFlushDrawable returns.
    CGLFlushDrawable(_context);
}

/**
 * Invalidates VLC objects (notably _vlc_vd)
 * This method must be called in VLCs module Close (or indirectly by the View)
 * to ensure all critical VLC resources that might be gone when the module is
 * closed are properly NULLed. This is necessary as dealloc is only called later
 * as it has to be done async on the main thread, because NSView must be
 * dealloc'ed on the main thread and the view own the layer, so the layer
 * will stay valid until the view is gone, and might still use _vlc_vd
 * even after the VLC module is gone and the resources would be invalid.
 */
- (void)vlcClose
{
    VLCCAOpenGLLayer *layer = (VLCCAOpenGLLayer *)[self layer];
    [layer vlcClose];

    @synchronized (layer) {
        _gl = NULL;

        // It should never happen that the context is destroyed and we
        // still have a previous context set, as it would mean non-balanced
        // calls to MakeCurrent/ReleaseCurrent.
        assert(_context_previous == NULL);
    }
    CGLReleaseContext(_context);

    dispatch_async(dispatch_get_main_queue(), ^{
        // Remove vout subview from container
        if ([_container respondsToSelector:@selector(removeVoutSubview:)]) {
            [_container removeVoutSubview:self];
        }
        [self removeFromSuperview];
    });
}

- (void)viewWillStartLiveResize
{
    [(VLCCAOpenGLLayer *)self.layer setAsynchronous:YES];
}

- (void)viewDidEndLiveResize
{
    [(VLCCAOpenGLLayer *)self.layer setAsynchronous:NO];
}

- (CALayer *)makeBackingLayer
{
    @synchronized(self) {
        NSAssert(_gl != NULL, @"Cannot create backing layer without vout display!");

        assert(_context != NULL);
        VLCCAOpenGLLayer *layer = [[VLCCAOpenGLLayer alloc] init:_gl context:_context];
        layer.delegate = self;
        return layer;
    }
}

/* Layer delegate method that ensures the layer always get the
 * correct contentScale based on whether the view is on a HiDPI
 * display or not, and when it is moved between displays.
 */
- (BOOL)layer:(CALayer *)layer
shouldInheritContentsScale:(CGFloat)newScale
   fromWindow:(NSWindow *)window
{
    return YES;
}

/*
 * General properties
 */

- (BOOL)isOpaque
{
    return YES;
}

@end

#pragma mark -
#pragma mark VLCCAOpenGLLayer

@implementation VLCCAOpenGLLayer

- (instancetype)init:(vlc_gl_t *)gl context:(CGLContextObj)context
{
    self = [super init];
    if (self) {
        _displayLock = [[NSLock alloc] init];
        _gl = gl;

        _glContext = CGLRetainContext(context);
        assert(_glContext != NULL);

        atomic_init(&_is_ready, false);

        [CATransaction lock];
        self.needsDisplayOnBoundsChange = YES;
        self.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        self.asynchronous = NO;
        self.opaque = 1.0;
        self.hidden = NO;
        [CATransaction unlock];
    }

    return self;
}

- (void)markReady {
    atomic_store(&_is_ready, true);
}

/**
 * Invalidates VLC objects (notably _voutDisplay)
 * This method must be called in VLCs module Close (or indirectly by the View).
 */
- (void)vlcClose
{
    @synchronized (self) {
        atomic_store(&_is_ready, false);
        _gl = NULL;
    }
}

- (void)dealloc
{
    CGLReleaseContext(_glContext);
}

- (void)display
{
    [_displayLock lock];

    [super display];
    [CATransaction flush];

    [_displayLock unlock];
}

- (void)displayFromVout
{
    if (self.asynchronous) {
        // During live resizing we do not take updates
        // from the vout, as those would interfere with
        // the rendering currently happening on the main
        // thread for the resize. Rendering anyway happens
        // triggered by the OS every display refresh, so
        // forcing an update here would be useless anyway.
        return;
    }

    [self display];
}

- (BOOL)canDrawInCGLContext:(CGLContextObj)glContext
                pixelFormat:(CGLPixelFormatObj)pixelFormat
               forLayerTime:(CFTimeInterval)timeInterval
                displayTime:(const CVTimeStamp *)timeStamp
{
    @synchronized(self) {
        if (!_gl)
            return NO;
        return _is_ready;
    }
}

- (void)drawInCGLContext:(CGLContextObj)glContext
             pixelFormat:(CGLPixelFormatObj)pixelFormat
            forLayerTime:(CFTimeInterval)timeInterval
             displayTime:(const CVTimeStamp *)timeStamp
{
    @synchronized(self) {
        if (!_gl)
            return;

        GLint dims[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, dims);
        NSSize newSize = NSMakeSize(dims[2], dims[3]);

        if (NSEqualSizes(newSize, NSZeroSize)) {
            newSize = self.bounds.size;
            CGFloat scale = self.contentsScale;
            newSize.width *= scale;
            newSize.height *= scale;
        }

        vlc_gl_ReportRender(_gl, newSize.width, newSize.height);

        // Copies a double-buffered contexts back buffer to front buffer, calling
        // glFlush before this is not needed and discouraged for performance reasons.
        // An implicit glFlush happens before CGLFlushDrawable returns.
        CGLFlushDrawable(_glContext);
    }
}

- (CGLPixelFormatObj)copyCGLPixelFormatForDisplayMask:(uint32_t)mask
{
    CGLPixelFormatObj fmt = CGLGetPixelFormat(_glContext);

    return (fmt) ? CGLRetainPixelFormat(fmt) : NULL;
}

- (CGLContextObj)copyCGLContextForPixelFormat:(CGLPixelFormatObj)pixelFormat
{
    return CGLRetainContext(_glContext);
}

@end

/*
 * Module descriptor
 */
vlc_module_begin()
    set_description(N_("Core Animation OpenGL Layer (Mac OS X)"))
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_callback_opengl(OpenOpenGL, 300)

    add_opengl_submodule_renderer()
vlc_module_end()
