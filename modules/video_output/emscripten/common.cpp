/*****************************************************************************
 * common.h: Emscripten decoder/vout common code
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

#include <vlc_common.h>
#include <vlc_decoder.h>
#include <vlc_picture_pool.h>

#include "common.h"

static picture_context_t* PictureContextCopy(picture_context_t* picCtx)
{
    vlc_video_context_Hold( picCtx->vctx );
    return picCtx;
}

static void PictureContextDestroy( picture_context_t* picCtx )
{
    auto ctx = static_cast<webcodec_context*>(
        vlc_video_context_GetPrivate( picCtx->vctx, VLC_VIDEO_CONTEXT_WEBCODEC ) );
    MAIN_THREAD_EM_ASM({
        const WEBCODEC_MAX_PICTURES = 32;
        const picIndex = $0;
        const poolIndex = $1;
        const frame = Module.framePools[poolIndex][picIndex];
        console.assert(picIndex >= 0 && picIndex < WEBCODEC_MAX_PICTURES);
        if (frame !== undefined)
            frame.close();
        Module.framePools[poolIndex][picIndex] = undefined;
    },  PictureContextPrivate(picCtx)->pictureIdx, ctx->decoder_worker );
}

static picture_context_t* webcodec_CreatePictureContext( vlc_video_context* vctx, uint32_t picIdx )
{
    auto picCtx = static_cast<webcodec_picture_ctx*>(
                malloc( sizeof(  webcodec_picture_ctx ) ) );
    if ( picCtx == nullptr )
        return nullptr;
    picCtx->context.copy = PictureContextCopy;
    picCtx->context.destroy = PictureContextDestroy;
    picCtx->context.vctx = vctx;
    picCtx->pictureIdx = picIdx;
    return &picCtx->context;
}


int webcodec_CreatePool( vlc_video_context* vctx, const video_format_t* fmt )
{
    auto ctx = static_cast<webcodec_context*>(
        vlc_video_context_GetPrivate( vctx, VLC_VIDEO_CONTEXT_WEBCODEC ) );

    for ( auto i = 0u; i < ARRAY_SIZE(ctx->pictures); ++i )
    {
        auto pic = picture_NewFromFormat(fmt);
        if ( pic != nullptr )
            pic->context = webcodec_CreatePictureContext( vctx, i );
        if ( pic == nullptr || pic->context == nullptr )
        {
            while ( i-- )
                picture_Release( ctx->pictures[i] );
            return VLC_EGENERIC;
        }
        ctx->pictures[i] = pic;
    }
    ctx->pool = picture_pool_New(ARRAY_SIZE(ctx->pictures), ctx->pictures);
    if ( ctx->pool == nullptr )
    {
        for ( auto i = 0u; i < ARRAY_SIZE(ctx->pictures); ++i )
            picture_Release( ctx->pictures[i] );
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}
