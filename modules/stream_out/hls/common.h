/*****************************************************************************
 * common.h: HLS common types and functions
 *****************************************************************************
 * Copyright (C) 2003-2011 VLC authors and VideoLAN
 *
 * Authors: Alaric Senat <dev.asenat@posteo.net>
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

#ifndef HLS_COMMON_H
#define HLS_COMMON_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>

#include <vlc_httpd.h>

#define SOUT_CFG_PREFIX "sout-hls-"
int SoutOpen( vlc_object_t * );
void SoutClose( vlc_object_t * );

#define ACO_CFG_PREFIX "aco-hls-"
int AccessOpen( vlc_object_t * );
void AccessClose( vlc_object_t * );

#define PACKED_MUX_CFG_PREFIX "mux-hls-packed-"
int PackedMuxOpen( vlc_object_t * );
void PackedMuxClose( vlc_object_t * );

#define VTT_MUX_CFG_PREFIX "mux-hls-vtt-"
int WebVTTMuxOpen( vlc_object_t * );
void WebVTTMuxClose( vlc_object_t * );

typedef struct hls_io hls_io;

/*
 * Create a new io descriptor.
 *
 * @param path Filesystem path. If NULL, the returned IO will be memory-only.
 * @param flags Open flags.
 */
VLC_EXTERN hls_io *hls_io_New( const char *path, int flags );

typedef struct
{
    /* Usual IO manipulations */
    int ( *open )( hls_io * );
    /* Write a block using the IO implementation, this function takes ownership
     * of the block.
     * Returns data not consumed (if any) in a new block */
    block_t *( *consume_block )( hls_io *, block_t * );
    void ( *close )( hls_io * );

    /* Return an opaque data structure used for multithreaded reads */
    void *( *new_reading_context )( const hls_io * );
    void ( *release_reading_context )( void * );
    ssize_t ( *read )( void *, uint8_t[], size_t );

    /* Destroy and/or unlink the IO representation, used to get rid of
     * segments content before actually releasing the data structure */
    int ( *unlink )( hls_io * );

    /* Release IO internal state */
    void ( *release )( hls_io * );
} hls_io_ops;

struct hls_io
{
    void *sys;   //< Opaque internal data structure.
    size_t size; //< Number of wrote bytes in the IO.

    hls_io_ops ops; //< IO Callbacks.
};

typedef struct
{
    char *psz_filename;
    char *psz_path;
    char *psz_uri;
    char *psz_key_uri;
    char *psz_duration;
    vlc_tick_t segment_length;
    vlc_tick_t segment_begin;
    uint32_t i_segment_number;
    uint8_t aes_ivs[16];
    hls_io *io_handle;
    bool closed;
} hls_segment;

typedef struct
{
    const hls_io *handle;
    vlc_mutex_t lock;
} hls_index;

#define HLS_SOUT_CALLBACKS_VAR "sout-hls-callbacks"
struct hls_sout_callbacks
{
    void *sys;

    int ( *segment_added )( void *, const hls_segment * );
    void ( *segment_removed )( void *, const hls_segment * );

    void ( *index_updated )( void *, const hls_io * );
};

int hls_http_SegmentCallback( httpd_callback_sys_t *sys,
                              httpd_client_t *cl,
                              httpd_message_t *answer,
                              const httpd_message_t *query );
int hls_http_IndexCallback( httpd_callback_sys_t *sys,
                            httpd_client_t *cl,
                            httpd_message_t *answer,
                            const httpd_message_t *query );

/*
 * Prepend an HLS specific id3 tag to a segment.
 *
 * @param data The segment first block. Will be reallocated.
 *
 * @return The new block with the ID3 tag prepended or NULL in case of realloc error
 */
block_t *id3_Add( block_t *data );

#endif
