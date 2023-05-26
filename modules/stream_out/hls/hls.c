/*****************************************************************************
 * hls.c: HLS stream output module
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

#include <vlc_common.h>

#include <vlc_block.h>
#include <vlc_httpd.h>
#include <vlc_list.h>
#include <vlc_memstream.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_tracer.h>

#include "hls.h"
#include "segments.h"
#include "storage.h"

typedef struct
{
    struct hls_config config;
    struct vlc_list playlists;
    httpd_host_t *http_host;

    vlc_tick_t first_pcr;
    vlc_tick_t last_pcr;
    vlc_tick_t last_segment;
} sout_stream_sys_t;

static vlc_tick_t total_out_len = 0;
static vlc_tick_t total_in_len = 0;

typedef struct
{
    const struct hls_config *config;

    sout_access_out_t *access;
    sout_mux_t *mux;
    sout_input_t *input;

    struct segment_buffer
    {
        block_t *begin;
        block_t **end;
        vlc_tick_t length;
    } ongoing_segment_data;
    hls_segment_queue_t segments;

    char *url;
    struct hls_storage *manifest;
    httpd_url_t *http_manifest;

    vlc_tick_t last_dts;
    vlc_tick_t first_dts;
    vlc_tick_t last_len;
    vlc_tick_t len;
    const char *name;
    struct vlc_list node;
} hls_playlist_t;

static inline void ResetSegmentBuffer(struct segment_buffer *buffer)
{
    buffer->begin = NULL;
    buffer->end = &buffer->begin;
    buffer->length = 0u;
}

int HTTPCallback(httpd_callback_sys_t *sys,
                 httpd_client_t *client,
                 httpd_message_t *answer,
                 const httpd_message_t *query)
{
    if (answer == NULL || query == NULL || client == NULL)
        return VLC_SUCCESS;

    const struct hls_storage *storage = (struct hls_storage *)sys;

    printf("fetching %s\n", query->psz_url);

    httpd_MsgAdd(answer, "Content-Type", "%s", storage->mime);
    httpd_MsgAdd(answer, "Cache-Control", "no-cache");

    answer->i_proto = HTTPD_PROTO_HTTP;
    answer->i_version = 0;
    answer->i_type = HTTPD_MSG_ANSWER;

    const ssize_t size = storage->get_content(storage, &answer->p_body);
    if (size != -1)
    {
        answer->i_body = size;
        answer->i_status = 200;
    }
    else
        answer->i_status = 500;

    if (httpd_MsgGet(query, "Connection") != NULL)
        httpd_MsgAdd(answer, "Connection", "close");
    httpd_MsgAdd(answer, "Content-Length", "%d", answer->i_body);

    return VLC_SUCCESS;
}

static struct hls_storage *
GeneratePlaylistManifest(const hls_playlist_t *playlist)
{
    struct vlc_memstream out;
    vlc_memstream_open(&out);

#define MANIFEST_ADD_TAG(fmt, ...)                                             \
    do                                                                         \
    {                                                                          \
        if (vlc_memstream_printf(&out, fmt "\n", ##__VA_ARGS__) < 0)           \
            goto error;                                                        \
    } while (0)

    MANIFEST_ADD_TAG("#EXTM3U");
    const double seg_duration =
        secf_from_vlc_tick(playlist->config->segment_length);
    MANIFEST_ADD_TAG("#EXT-X-TARGETDURATION:%.0f", seg_duration);
    MANIFEST_ADD_TAG("#EXT-X-VERSION:3");
    MANIFEST_ADD_TAG("#EXT-X-ALLOW-CACHE:NO");
    MANIFEST_ADD_TAG("#EXT-X-PLAYLIST-TYPE:EVENT");

    const hls_segment_t *first_seg = hls_segment_GetFirst(&playlist->segments);
    MANIFEST_ADD_TAG("#EXT-X-MEDIA-SEQUENCE:%u",
                     (first_seg == NULL) ? 0u : first_seg->id);

    const hls_segment_t *segment;
    hls_segment_queue_Foreach(&playlist->segments, segment)
    {
        MANIFEST_ADD_TAG("#EXTINF:%.2f,", secf_from_vlc_tick(segment->length));
        MANIFEST_ADD_TAG("%s", segment->url);
    }

#undef MANIFEST_ADD_TAG

    if (vlc_memstream_close(&out) != 0)
        return NULL;
    printf("%.*s\n", (int)out.length, out.ptr);
    return hls_storage_FromBytes(out.ptr,
                                 out.length,
                                 "application/vnd.apple.mpegurl",
                                 HLS_STORAGE_TYPE_MEMORY);
error:
    if (vlc_memstream_close(&out) != 0)
        return NULL;
    free(out.ptr);
    return NULL;
}

//static ssize_t AccessOutWrite(sout_access_out_t *access, block_t *block)
//{
//    hls_playlist_t *playlist = access->p_sys;
//
//    ssize_t wrote = 0;
//    while (block != NULL)
//    {
//        if (playlist->ongoing_segment_data.length + block->i_length >=
//            playlist->config->segment_length)
//        {
//            const int status = hls_segment_queue_NewSegment(
//                &playlist->segments,
//                playlist->ongoing_segment_data.begin,
//                playlist->ongoing_segment_data.length);
//            if (unlikely(status != VLC_SUCCESS))
//                goto error;
//
//            struct hls_storage *new_manifest =
//                GeneratePlaylistManifest(playlist);
//            if (unlikely(new_manifest == NULL))
//                goto error;
//
//            httpd_UrlCatch(playlist->http_manifest,
//                           HTTPD_MSG_GET,
//                           HTTPCallback,
//                           (httpd_callback_sys_t *)new_manifest);
//
//            if (playlist->manifest != NULL)
//                hls_storage_Destroy(playlist->manifest);
//            playlist->manifest = new_manifest;
//
//            ResetSegmentBuffer(&playlist->ongoing_segment_data);
//        }
//
//        wrote += block->i_buffer;
//
//        block_t *next = block->p_next;
//
//        block->p_next = NULL;
//        playlist->ongoing_segment_data.length += block->i_length;
//        block_ChainLastAppend(&playlist->ongoing_segment_data.end, block);
//
//        block = next;
//    }
//
//    return wrote;
//error:
//    block_ChainRelease(block);
//    return -1;
//}

static ssize_t AccessOutWrite(sout_access_out_t *access, block_t *block)
{
    hls_playlist_t *playlist = access->p_sys;

    if (playlist->first_dts == VLC_TICK_INVALID)
        playlist->first_dts = block->i_dts;

    ssize_t wrote = 0;
    while (block != NULL)
    {
        wrote += block->i_buffer;

        total_out_len += block->i_length;
        block_t *next = block->p_next;

        block->p_next = NULL;
        playlist->ongoing_segment_data.length += block->i_length;
        block_ChainLastAppend(&playlist->ongoing_segment_data.end, block);

        block = next;
    }

    return wrote;
}

static sout_access_out_t *CreateAccessOut(sout_stream_t *stream,
                                          hls_playlist_t *sys)
{
    sout_access_out_t *access = vlc_object_create(stream, sizeof(*access));
    if (unlikely(access == NULL))
        return NULL;

    access->p_cfg = NULL;
    access->p_module = NULL;
    access->p_sys = sys;
    access->psz_access = strdup("hls");
    access->psz_path = NULL;

    access->pf_control = NULL;
    access->pf_read = NULL;
    access->pf_seek = NULL;
    access->pf_write = AccessOutWrite;
    return access;
}

static hls_playlist_t *CreatePlaylist(sout_stream_t *stream)
{
    hls_playlist_t *playlist = calloc(1, sizeof(*playlist));
    if (unlikely(playlist == NULL))
        return NULL;
    return playlist;
}

static inline char *FormatPlaylistManifestURL(const hls_playlist_t *playlist)
{
    char *ret;
    const int status = asprintf(&ret,
                                "%s/playlist-%u-index.m3u8",
                                playlist->config->base_url,
                                playlist->input->fmt.i_id);
    if (unlikely(status == -1))
        return NULL;
    return ret;
}

static void *Add(sout_stream_t *stream, const es_format_t *fmt)
{
    sout_stream_sys_t *sys = stream->p_sys;

    if (fmt->i_cat == AUDIO_ES)
        return NULL;

    hls_playlist_t *playlist = CreatePlaylist(stream);

    playlist->access = CreateAccessOut(stream, playlist);
    if (unlikely(playlist->access == NULL))
        goto error;

    playlist->mux = sout_MuxNew(playlist->access, "ts");
    if (unlikely(playlist->mux == NULL))
        goto error;

    playlist->input = sout_MuxAddStream(playlist->mux, fmt);
    if (playlist->input == NULL)
        goto error;

    playlist->config = &sys->config;

    playlist->url = FormatPlaylistManifestURL(playlist);
    if (unlikely(playlist->url == NULL))
        goto error;

    hls_segment_queue_Init(
        &playlist->segments, fmt->i_id, sys->http_host, &sys->config);

    ResetSegmentBuffer(&playlist->ongoing_segment_data);

    playlist->manifest = NULL;
    playlist->http_manifest =
        httpd_UrlNew(sys->http_host, playlist->url, NULL, NULL);
    if (playlist->http_manifest == NULL)
        goto error;

    vlc_list_append(&playlist->node, &sys->playlists);

    playlist->last_dts = VLC_TICK_INVALID;
    playlist->last_len = VLC_TICK_INVALID;
    playlist->first_dts = VLC_TICK_INVALID;
    playlist->len = 0;
     static const char *const track_types[] = {
            [VIDEO_ES] = "VIDEO",
            [AUDIO_ES] = "AUDIO",
            [SPU_ES] = "SUBTITLES",
        };
     playlist->name = track_types[fmt->i_cat];

     printf("%s IS %d\n", track_types[fmt->i_cat], fmt->i_id);

    return playlist;
error:
    if (playlist->access != NULL)
        sout_AccessOutDelete(playlist->access);
    if (playlist->input != NULL)
        sout_MuxDeleteStream(playlist->mux, playlist->input);
    if (playlist->mux != NULL)
        sout_MuxDelete(playlist->mux);
    free(playlist->url);
    free(playlist);
    return NULL;
}

static void Del(sout_stream_t *stream, void *id)
{
    hls_playlist_t *playlist = id;

    sout_MuxDeleteStream(playlist->mux, playlist->input);
    sout_MuxDelete(playlist->mux);

    sout_AccessOutDelete(playlist->access);

    httpd_UrlDelete(playlist->http_manifest);

    if (playlist->manifest != NULL)
        hls_storage_Destroy(playlist->manifest);

    block_ChainRelease(playlist->ongoing_segment_data.begin);
    hls_segment_queue_Clear(&playlist->segments);
    
    free(playlist->url);

    vlc_list_remove(&playlist->node);
    free(playlist);
}

static int Send(sout_stream_t *stream, void *id, vlc_frame_t *frame)
{
    hls_playlist_t *playlist = id;
    for (vlc_frame_t *f = frame; f != NULL; f = f->p_next)
    {
        playlist->last_dts = f->i_dts;
        playlist->last_len = f->i_length;
        playlist->len += f->i_length;
        total_in_len += f->i_length;

        struct vlc_tracer *tracer = vlc_object_get_tracer(VLC_OBJECT(stream));
        if (tracer != NULL)
        {
            vlc_tracer_TraceStreamDTS(
                tracer, "HLS", playlist->name, "IN", f->i_pts, f->i_dts);
            if (f->i_flags & BLOCK_FLAG_TYPE_I)
            {
                char *n;
                asprintf(&n, "%s-iframe", playlist->name);
                vlc_tracer_TraceStreamDTS(
                    tracer, "HLS", n, "OUT", f->i_pts, f->i_dts);
                free(n);
            }
        }
    }
    return sout_MuxSendBuffer(playlist->mux, playlist->input, frame);
}

static void SetPCR(sout_stream_t *stream, vlc_tick_t pcr)
{
    sout_stream_sys_t *sys = stream->p_sys;

    struct vlc_tracer *tracer = vlc_object_get_tracer(VLC_OBJECT(stream));
    if (tracer != NULL)
    {
        vlc_tracer_TracePCR(tracer, "HLS", "IN", pcr);
    }
    if (sys->first_pcr == VLC_TICK_INVALID)
    {
        sys->first_pcr = pcr;
        sys->last_pcr = pcr;
        return;
    }

    printf("INTERVAL %ld\n", MS_FROM_VLC_TICK(pcr - sys->last_pcr));
    sys->last_pcr = pcr;
    const vlc_tick_t stream_time =  pcr- sys->first_pcr;
    //printf("fpcr = %ld\n", sys->first_pcr);
    printf("st = %ld\n", stream_time);
    if (stream_time - sys->last_segment > sys->config.segment_length) 
    {
    struct vlc_tracer *tracer = vlc_object_get_tracer(VLC_OBJECT(stream));
    if (tracer != NULL)
    {
        vlc_tracer_TracePCR(tracer, "SEGMENT", "IN", pcr);
    }
        hls_playlist_t *playlist;
        vlc_list_foreach(playlist, &sys->playlists, node)
        {
            //printf("last %d: %ld %ld %ld\n",
            //       playlist->input->fmt.i_id,
            //       playlist->last_dts,
            //       playlist->last_dts + playlist->last_len,
            //       playlist->len);

            block_t *chain = playlist->ongoing_segment_data.begin;
            block_t *begin = playlist->ongoing_segment_data.begin;
            vlc_tick_t s = 0;
            block_t *last = NULL;
            int i = 0;
            for (; chain != NULL; chain = chain->p_next)
            {
                //if (i % 100 == 0)
                //    printf("s = %ld dts = %ld\n", s, chain->i_dts - playlist->first_dts);
                const vlc_tick_t u = ((chain->i_dts + chain->i_length) - playlist->first_dts) - sys->last_segment; 
                if (u > sys->config.segment_length)
                {
                    playlist->ongoing_segment_data.begin = chain;
                    if (last != NULL)
                        last->p_next = NULL;
                    break;
                }
                s = u;
                last = chain;
            }

            if (chain == NULL)
                ResetSegmentBuffer(&playlist->ongoing_segment_data);

            const int status = hls_segment_queue_NewSegment( &playlist->segments, begin,s);
            if (unlikely(status != VLC_SUCCESS))
                return;

            struct hls_storage *new_manifest =
                GeneratePlaylistManifest(playlist);
            if (unlikely(new_manifest == NULL))
                return;

            httpd_UrlCatch(playlist->http_manifest,
                           HTTPD_MSG_GET,
                           HTTPCallback,
                           (httpd_callback_sys_t *)new_manifest);

            if (playlist->manifest != NULL)
                hls_storage_Destroy(playlist->manifest);
            playlist->manifest = new_manifest;

            vlc_tick_t size;
            int count;
            block_ChainProperties(playlist->ongoing_segment_data.begin, &count, NULL, &size);
            printf("size %d: %zu\n", playlist->input->fmt.i_id, MS_FROM_VLC_TICK(size));
            printf("count %d: %d\n", playlist->input->fmt.i_id, count);

            size = block_FifoSize((*playlist->mux->pp_inputs)->p_fifo );
            block_t *b = vlc_fifo_Show((*playlist->mux->pp_inputs)->p_fifo);
            printf("fifo size %d: %zu\n", playlist->input->fmt.i_id, b->i_length);
            //ResetSegmentBuffer(&playlist->ongoing_segment_data);
        }
        sys->last_segment = stream_time;
    }
}

static int Control(sout_stream_t *stream, int query, va_list args)
{
    const sout_stream_sys_t *sys = stream->p_sys;
    switch (query)
    {
        case SOUT_STREAM_IS_SYNCHRONOUS:
            *va_arg(args, bool *) = sys->config.pace;
            break;

        default:
            return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

#define SOUT_CFG_PREFIX "sout-hls-"

static int Open(vlc_object_t *this)
{
    sout_stream_t *stream = (sout_stream_t *)this;

    sout_stream_sys_t *sys = malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    static const char *const options[] = {
        "base-url", "numsegs", "pace", "seglen", NULL};
    config_ChainParse(stream, SOUT_CFG_PREFIX, options, stream->p_cfg);

    sys->http_host = vlc_http_HostNew(this);
    if (sys->http_host == NULL)
    {
        free(sys);
        return VLC_EGENERIC;
    }

    sys->config.base_url = var_GetString(stream, SOUT_CFG_PREFIX "base-url");
    sys->config.max_segments =
        var_GetInteger(stream, SOUT_CFG_PREFIX "numsegs");
    sys->config.pace = var_GetBool(stream, SOUT_CFG_PREFIX "pace");
    sys->config.segment_length =
        VLC_TICK_FROM_SEC(var_GetInteger(stream, SOUT_CFG_PREFIX "seglen"));

    sys->first_pcr = VLC_TICK_INVALID;
    sys->last_segment = 0;

    vlc_list_init(&sys->playlists);

    static const struct sout_stream_operations ops = {
        .add = Add,
        .del = Del,
        .send = Send,
        .set_pcr = SetPCR, 
        .control = Control,
    };
    stream->ops = &ops;
    stream->p_sys = sys;

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *this)
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;

    printf("stats: in %ld out %ld st %ld\n",
           MS_FROM_VLC_TICK(total_in_len),
           MS_FROM_VLC_TICK(total_out_len),
           MS_FROM_VLC_TICK(sys->last_segment));
    hls_config_Clean(&sys->config);

    free(sys);
}

#define BASEURL_TEXT N_("Base URL/Path")
#define NUMSEGS_LONGTEXT N_("Number of segments to include in index")
#define NUMSEGS_TEXT N_("Number of segments")
#define PACE_LONGTEXT                                                          \
    N_("Enable input pacing, the media will play at a playbak rate")
#define PACE_TEXT N_("Enable pacing")
#define SEGLEN_LONGTEXT N_("Length of segments in seconds")
#define SEGLEN_TEXT N_("Segment length (sec)")

// clang-format off
vlc_module_begin()
    set_shortname("HLS")
    set_description(N_("HLS stream output"))
    set_capability("sout output", 50)
    add_shortcut("hls")
    set_subcategory(SUBCAT_SOUT_STREAM)

    add_string(SOUT_CFG_PREFIX "base-url", "", BASEURL_TEXT, BASEURL_TEXT)
    add_integer(SOUT_CFG_PREFIX "numsegs", 5, NUMSEGS_TEXT, NUMSEGS_LONGTEXT)
    add_bool(SOUT_CFG_PREFIX "pace", true, PACE_TEXT, PACE_LONGTEXT)
    add_integer(SOUT_CFG_PREFIX "seglen", 4, SEGLEN_TEXT, SEGLEN_LONGTEXT)

    set_callbacks(Open, Close)
vlc_module_end()
