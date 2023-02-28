/*****************************************************************************
 * storage.h
 *****************************************************************************
 * Copyright (C) 2023 VLC authors and VideoLAN
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
#ifndef HLS_STORAGE_H
#define HLS_STORAGE_H

enum hls_storage_type
{
    HLS_STORAGE_TYPE_MEMORY,
};

typedef struct hls_storage
{
    enum hls_storage_type type;
    ssize_t (*get_content)(struct hls_storage *, uint8_t **dest);
} hls_storage_t;

hls_storage_t *hls_storage_FromBlocks(block_t *content, enum hls_storage_type);

void hls_storage_Destroy(hls_storage_t *);

#endif
