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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>

#include <vlc_common.h>

#include <vlc_block.h>

#include "storage.h"

struct mem_storage_priv
{
    hls_storage_t storage;
    block_t *content;
    size_t size;
};

static ssize_t mem_storage_GetContent(const hls_storage_t *storage,
                                      uint8_t **dest)
{
    const struct mem_storage_priv *priv =
        container_of(storage, struct mem_storage_priv, storage);

    *dest = malloc(priv->size);
    if (unlikely(*dest == NULL))
        return -1;

    uint8_t *cursor = *dest;
    for (const block_t *it = priv->content; it != NULL; it = it->p_next)
    {
        memcpy(cursor, it->p_buffer, it->i_buffer);
        cursor += it->i_buffer;
    }
    return priv->size;
}

static hls_storage_t *mem_storage_FromBlock(block_t *content)
{
    struct mem_storage_priv *priv = malloc(sizeof(*priv));

    priv->storage.type = HLS_STORAGE_TYPE_MEMORY;
    priv->storage.get_content = mem_storage_GetContent;
    priv->content = content;
    block_ChainProperties(content, NULL, &priv->size, NULL);
    return &priv->storage;
}

static hls_storage_t *mem_storage_FromBytes(void *bytes, size_t size)
{
    struct mem_storage_priv *priv = malloc(sizeof(*priv));

    block_t *content = block_heap_Alloc(bytes, size);
    if (unlikely(content == NULL))
    {
        free(priv);
        return NULL;
    }

    priv->storage.type = HLS_STORAGE_TYPE_MEMORY;
    priv->storage.get_content = mem_storage_GetContent;
    priv->content = content;
    priv->size = size;
    return &priv->storage;
}

static void mem_storage_Destroy(hls_storage_t *storage)
{
    struct mem_storage_priv *priv =
        container_of(storage, struct mem_storage_priv, storage);

    block_ChainRelease(priv->content);
    free(priv);
}

hls_storage_t *hls_storage_FromBlocks(block_t *content,
                                      const char *mime,
                                      enum hls_storage_type type)
{
    hls_storage_t *storage;
    switch (type)
    {
        case HLS_STORAGE_TYPE_MEMORY:
            storage = mem_storage_FromBlock(content);
            break;
        default:
            vlc_assert_unreachable();
    }

    if (storage != NULL)
        storage->mime = mime;
    return storage;
}

hls_storage_t *hls_storage_FromBytes(void *data,
                                     size_t size,
                                     const char *mime,
                                     enum hls_storage_type type)
{
    hls_storage_t *storage;
    switch (type)
    {
        case HLS_STORAGE_TYPE_MEMORY:
            storage = mem_storage_FromBytes(data, size);
            break;
        default:
            vlc_assert_unreachable();
    }

    if (storage != NULL)
        storage->mime = mime;
    return storage;
}

void hls_storage_Destroy(hls_storage_t *storage)
{
    switch (storage->type)
    {
        case HLS_STORAGE_TYPE_MEMORY:
            mem_storage_Destroy(storage);
            break;
        default:
            vlc_assert_unreachable();
    }
}
