/*****************************************************************************
 * common.h: HLS common types and functions
 *****************************************************************************
 * Copyright (C) 2022 VLC authors and VideoLAN
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

#define ACCESS_CFG_PREFIX "access-hls-"
int AccessOpen( vlc_object_t * );
void AccessClose( vlc_object_t * );

typedef struct hls_storage hls_storage_t;
struct hls_storage
{
    void *sys;
    size_t size;

    block_t *(*write_block)(hls_storage_t *, block_t *);
    ssize_t (*write_bytes)(hls_storage_t *, const uint8_t *, size_t);
    int (*read)(const hls_storage_t *, uint8_t[], size_t);
    void (*destroy)(hls_storage_t *);
};

hls_storage_t *
hls_storage_FromFile(const char *path, int flags, bool unlink_on_destroy);
hls_storage_t *hls_storage_MemoryNew(void);
void hls_storage_Destroy(hls_storage_t *storage);


#endif
