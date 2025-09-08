/*****************************************************************************
 * webcodec.cpp: VLC picture to WebCodec VideoFrame
 *****************************************************************************
 * Copyright (C) 2022 VLC authors, VideoLAN and VideoLabs
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

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_plugin.h>

#include "../../video_output/emscripten/common.h"

#include <emscripten/emscripten.h>
#include <emscripten/em_js.h>
#undef typeof // FIXME Remove once val::typeof is finally removed
#include <emscripten/val.h>

struct chroma_sys_t
{
};

static picture_t* UploadSurface(filter_t* filter, picture_t* src)
{
    auto vctx = static_cast<webcodec_context*>(
            vlc_video_context_GetPrivate( filter->vctx_out, VLC_VIDEO_CONTEXT_WEBCODEC) );
    auto dst = picture_pool_Wait( vctx->pool );
    if (dst == nullptr)
    {
        picture_Release(src);
        return nullptr;
    }

    auto size = 0u;
    for ( auto i = 0; i < src->i_planes; ++i )
        size += src->p[i].i_lines * src->p[i].i_pitch;
    auto buffer = emscripten::val::global("Uint8Array").new_( size );
    auto planesLayout = emscripten::val::array();
    auto offset = 0u;
    for ( auto i = 0; i < src->i_planes; ++i )
    {
        auto planeSize = src->p[i].i_lines * src->p[i].i_pitch;
        auto p = emscripten::typed_memory_view( planeSize, src->p[i].p_pixels );
        buffer.call<void>("set", p, offset);
        auto layout = emscripten::val::object();
        layout.set("offset", offset);
        layout.set("stride", src->p[i].i_pitch);
        planesLayout.call<void>( "push", layout );
        offset += planeSize;
    }

    auto init = emscripten::val::object();
    init.set( "format", "I420" );
    init.set( "codedWidth", src->format.i_width );
    init.set( "codedHeight", src->format.i_height );
    init.set( "timestamp", (long)US_FROM_VLC_TICK( src->date ) );
    init.set( "layout", planesLayout );
    init.set( "displayWidth", src->format.i_visible_width);
    init.set( "displayHeight", src->format.i_visible_height );

    auto visibleRect = emscripten::val::object();
    visibleRect.set( "x", src->format.i_x_offset );
    visibleRect.set( "y", src->format.i_y_offset );
    visibleRect.set( "width", src->format.i_visible_width);
    visibleRect.set( "height", src->format.i_visible_height );
    init.set( "visibleRect", visibleRect );

    auto frame = emscripten::val::global("VideoFrame").new_( buffer, init );
    auto pictureIdx = PictureContextPrivate(dst->context)->pictureIdx;
    EM_ASM({
        let frame = Emval.toValue($0);
        Module.msgChannel.port2.postMessage({
            customCmd: 'displayFrame',
            pictureId: $1,
            frame: frame
        });
    }, frame.as_handle(), pictureIdx);


    picture_CopyProperties( dst, src );
    picture_Release(src);
    return dst;
}

static void vlc_webcodec_CloseChroma(filter_t* filter)
{
    auto vctx = static_cast<webcodec_context*>(
                vlc_video_context_GetPrivate(filter->vctx_out, VLC_VIDEO_CONTEXT_WEBCODEC ) );
    picture_pool_Release(vctx->pool);
}

static const struct vlc_filter_operations filter_ops = {
    .filter_video = UploadSurface,   .close = vlc_webcodec_CloseChroma,
};

static int vlc_webcodec_OpenChroma(filter_t* filter)
{
    if (filter->fmt_in.video.i_height != filter->fmt_out.video.i_height
     || filter->fmt_in.video.i_width != filter->fmt_out.video.i_width
     || filter->fmt_in.video.orientation != filter->fmt_out.video.orientation)
    {
        return VLC_EGENERIC;
    }

    if (filter->fmt_out.video.i_chroma != VLC_CODEC_WEBCODEC_OPAQUE)
        return VLC_EGENERIC;
    if (filter->fmt_in.video.i_chroma != VLC_CODEC_I420)
        return VLC_EGENERIC;

    filter->ops = &filter_ops;
    return webcodec_CreatePool(filter->vctx_out, &filter->fmt_out.video);
}

vlc_module_begin()
    set_shortname(N_("WebCodec filters"))
    set_description(N_("WebCodec filters"))
    set_subcategory(SUBCAT_VIDEO_VFILTER)

    add_submodule()
    set_callback_video_converter(vlc_webcodec_OpenChroma, 10)
vlc_module_end()
