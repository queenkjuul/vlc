/*****************************************************************************
 * emjsfetch.c: Emscripten fetch access
 *****************************************************************************
 * Copyright © 2024 Videolabs
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

#include <vlc_access.h>
#include <vlc_plugin.h>

#include <emscripten/fetch.h>

typedef struct
{
    uint64_t offset;
    uint64_t size;
} access_sys_t;

static int Control(stream_t *access, int query, va_list args)
{
    access_sys_t *sys = access->p_sys;

    switch (query)
    {
        case STREAM_CAN_SEEK:
            *va_arg(args, bool *) = true;
            break;

        case STREAM_CAN_FASTSEEK:
            *va_arg(args, bool *) = false;
            break;
        case STREAM_GET_SIZE:
        {
            printf("GET SIZE %lld\n", sys->size);
            if (sys->size == 0)
                return -ENOENT;

            *va_arg(args, uint64_t *) = sys->size;
            break;
        }

        case STREAM_CAN_PAUSE:
        case STREAM_CAN_CONTROL_PACE:
            *va_arg(args, bool *) = true;
            break;

        case STREAM_GET_PTS_DELAY:
            *va_arg(args, vlc_tick_t *) =
                VLC_TICK_FROM_MS(var_InheritInteger(access, "network-caching"));
            break;

        default:
            return -ENOTSUP;
    }
    return VLC_SUCCESS;
}

static block_t *Read(stream_t *access, bool *restrict eof)
{
    access_sys_t *sys = access->p_sys;

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "GET");
    attr.attributes =
        EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;

#define BLOCK_SIZE (2000000u)
    const uint64_t from = sys->offset;
    uint64_t to = sys->offset + BLOCK_SIZE;
    if (to > sys->size)
    {
        *eof = true;
        to = sys->size;
        if (from == to)
            return NULL;
    }

    char *range;
    if (asprintf(&range, "bytes=%" PRIu64 "-%" PRIu64, from, to - 1u) == -1)
        return NULL;

    char *creds_header = var_InheritString(access, "fetch-creds-header");
    char *creds_token = var_InheritString(access, "fetch-creds-token");
    if (creds_token != NULL)
    {
        const char *headers[] = {
            "Range",
            range,
            creds_header,
            creds_token,
            NULL,
        };
        attr.requestHeaders = headers;
    }
    else
    {
        const char *headers[] = {
            "Range",
            range,
            NULL,
        };
        attr.requestHeaders = headers;
    }

    attr.timeoutMSecs = 5;
    emscripten_fetch_t *fetch = emscripten_fetch(&attr, access->psz_url);
    if (unlikely(fetch == NULL))
    {
        msg_Err(access,
                "Fetch failed, probably a timeout (%us)",
                attr.timeoutMSecs);
        *eof = true;
        free(range);
        return NULL;
    }

    block_t *b;
    if (fetch->status == 200 || fetch->status == 206)
    {
        b = block_Alloc(fetch->numBytes);
        if (unlikely(b == NULL))
            goto out;

        memcpy(b->p_buffer, fetch->data, b->i_buffer);
        sys->offset += b->i_buffer;
    }
    else
    {
        *eof = true;
        b = NULL;
    }

out:
#undef BLOCK_SIZE
    emscripten_fetch_close(fetch);
    free(range);
    free(creds_header);
    free(creds_token);
    return b;
}

static int Seek(stream_t *access, uint64_t offset)
{
    access_sys_t *sys = access->p_sys;
    sys->offset = offset;
    return VLC_SUCCESS;
}

static void HeadRequest(stream_t *access)
{
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "HEAD");
    attr.attributes =
        EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;
    char *creds_header = var_InheritString(access, "fetch-creds-header");
    char *creds_token = var_InheritString(access, "fetch-creds-token");
    if (creds_token != NULL)
    {
        const char *headers[] = {
            creds_header,
            creds_token,
            NULL,
        };
        attr.requestHeaders = headers;
    }
    attr.timeoutMSecs = 5;
    emscripten_fetch_t *fetch = emscripten_fetch(&attr, access->psz_url);

    free(creds_header);
    free(creds_token);

    if (unlikely(fetch == NULL))
    {
        msg_Err(access, "HEAD fetch failed");
        return;
    }

    if (fetch->status != 200)
    {
        msg_Err(access, "HEAD fetch failed (%d)", fetch->status);
        return;
    }

    const size_t size = emscripten_fetch_get_response_headers_length(fetch);
    char *headers = malloc(size + 1);
    emscripten_fetch_get_response_headers(fetch, headers, size + 1);
    char **unpacked = emscripten_fetch_unpack_response_headers(headers);

    access_sys_t *sys = access->p_sys;
    for (; *unpacked != NULL; ++unpacked)
    {
        if (!strcmp(*unpacked, "content-length"))
        {
            const char *length = *(unpacked + 1);
            if (length != NULL)
                sys->size = strtoull(length, NULL, 10);
            break;
        }
    }
    emscripten_fetch_free_unpacked_response_headers(unpacked);
    free(headers);
}

static int Open(vlc_object_t *obj)
{
    stream_t *access = (stream_t *)obj;

    access_sys_t *sys = malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
        return -ENOMEM;

    sys->offset = 0u;
    sys->size = 0u;

    access->pf_block = Read;
    access->pf_seek = Seek;
    access->pf_control = Control;
    access->p_sys = sys;

    HeadRequest(access);
    if (sys->size == 0)
        msg_Warn(access,
                 "Could not access the resource length: header "
                 "'Content-Length' not valid/accessible");

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    stream_t *access = (stream_t *)obj;
    access_sys_t *sys = access->p_sys;
    free(sys);
}

#define CREDS_HEADER_TEXT N_("Credential header name")
#define CREDS_HEADER_LONGTEXT N_("Header name used to pass credentials")

#define CREDS_TOKEN_TEXT N_("Credential token")

// clang-format off
vlc_module_begin()
    set_description(N_("todo"))
    set_shortname(N_("fetch"))
    set_subcategory(SUBCAT_INPUT_ACCESS)
    set_capability("access", 42)
    add_shortcut("https", "http")

    add_string("fetch-creds-header", "X-VLC-UserData", CREDS_HEADER_TEXT, CREDS_HEADER_LONGTEXT)
    add_string("fetch-creds-token", CREDS_TOKEN_TEXT, CREDS_TOKEN_TEXT, NULL)

    set_callbacks(Open, Close)
vlc_module_end()
