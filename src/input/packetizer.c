/*****************************************************************************
 * packetizer.c:
 *****************************************************************************
 * Copyright (C) 1999-2019 VLC authors, VideoLAN and Videolabs SAS
 *
 * Authors:
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
#include "config.h"
#endif

#include <vlc_common.h>

#include <vlc_codec.h>
#include <vlc_decoder.h>
#include <vlc_modules.h>

#include "libvlc.h"
#include "packetizer.h"

decoder_t *vlc_input_packetizer_New( vlc_object_t *parent, const es_format_t *restrict fmt )
{
    decoder_t *packetizer = vlc_custom_create( parent, sizeof( decoder_t ), "packetizer" );
    if ( unlikely( packetizer == NULL ) )
        return NULL;

    decoder_Init( packetizer, fmt );
    packetizer->p_module = module_need_var( packetizer, "packetizer", "packetizer" );
    if ( unlikely( packetizer->p_module == NULL ) )
    {
        decoder_Clean( packetizer );
        vlc_object_delete( packetizer );
        return NULL;
    }
    return packetizer;
}

void vlc_input_packetizer_Delete( decoder_t *packetizer ) { decoder_Destroy( packetizer ); }

vlc_frame_t *vlc_input_packetizer_PacketizeFrame( decoder_t *packetizer, vlc_frame_t **frame )
{
    assert( packetizer->pf_packetize != NULL );

    return packetizer->pf_packetize( packetizer, frame );
}

void vlc_input_packetizer_Flush( decoder_t *packetizer )
{
    if ( packetizer->pf_flush != NULL )
        packetizer->pf_flush( packetizer );
}
