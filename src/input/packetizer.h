/*****************************************************************************
 * packetizer.h: Input packetizer functions
 *****************************************************************************
 * Copyright (C) 1998-2008 VLC authors and VideoLAN
 *
 * Authors: ...
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

#ifndef INPUT_PACKETIZER_H
#define INPUT_PACKETIZER_H

#include <vlc_common.h>
#include <vlc_frame.h>

decoder_t *vlc_input_packetizer_New( vlc_object_t *, const es_format_t *restrict fmt ) VLC_USED;
void vlc_input_packetizer_Delete( decoder_t * );

vlc_frame_t *vlc_input_packetizer_PacketizeFrame( decoder_t *, vlc_frame_t ** );
void vlc_input_packetizer_Flush( decoder_t * );
#endif
