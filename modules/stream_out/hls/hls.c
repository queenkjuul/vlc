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
#include <vlc_vector.h>

#include "../codec/hxxx_helper.h"
#include "hls.h"
#include "segments.h"
#include "storage.h"

/**
 * Represent one HLS playlist as in RFC 8216 section 4.
 */
typedef struct
{
    unsigned int id;

    const struct hls_config *config;

    sout_access_out_t *access;
    sout_mux_t *mux;
    /** Every ES muxed in this playlist. */
    struct vlc_list tracks;

    /**
     * Current unfinished segment buffer.
     */
    struct segment_buffer
    {
        block_t *begin;
        block_t **end;
        vlc_tick_t length;
    } ongoing_segment_data;

    /**
     * Completed segments queue.
     *
     * The queue is generally max-sized (configurable by the user). Which means
     * that, when the max size is reached, pushing in the queue erase the first
     * segment.
     */
    hls_segment_queue_t segments;

    char *url;

    /**
     * Current playlist manifest as in RFC 8216 section 4.3.3.
     */
    struct hls_storage *manifest;
    httpd_url_t *http_manifest;

    struct vlc_list node;
} hls_playlist_t;

static inline void ResetSegmentBuffer(struct segment_buffer *buffer)
{
    buffer->begin = NULL;
    buffer->end = &buffer->begin;
    buffer->length = 0u;
}

/**
 * Represent one ES.
 *
 * Returned from `pf_add` to have both the sout_input context and the owning
 * playlist reference.
 */
typedef struct
{
    sout_input_t *input;
    hls_playlist_t *playlist_ref;
    struct vlc_list node;
} hls_track_t;

/**
 * Represent parsed user-specification of "EXT-X-STREAM-INF" (As in RFC 8216
 * section 4.3.4.2) renditions.
 *
 * They basically are groups of tracks muxed together. Each one of these
 * represent one rendition of the main stream.
 */
struct stream_inf_desc
{
    /** Es id list specified by the plugin user. */
    struct VLC_VECTOR(unsigned int) es_list;
    /**
     * Reference on the playlist associated with this list of es. Having a
     * reference here after the playlist creation ease subsequent searches of
     * the playlist.
     */
    hls_playlist_t *playlist_ref;
};

typedef struct
{
    /** All the plugin constants. */
    struct hls_config config;

    httpd_host_t *http_host;

    struct VLC_VECTOR(struct stream_inf_desc *) stream_inf_descriptors;
    /**
     * All the created "EXT-X-STREAM-INF" (As in RFC 8216 section 4.3.4.2)
     * playlists.
     */
    struct vlc_list stream_inf_playlists;
    /**
     * All the created "EXT-X-MEDIA" (As in RFC 8216 section 4.3.4.1) playlists.
     */
    struct vlc_list media_playlists;

    /**
     * Total number of playlist created by the plugin.
     *
     * Notably used to create unique playlists IDs.
     */
    unsigned int playlist_created_count;

    /**
     * Current "Master" Playlist manifest (As in RFC 8216 4.3.4).
     */
    struct hls_storage *manifest;
    httpd_url_t *http_manifest;
} sout_stream_sys_t;

static inline hls_track_t *MediaGetTrack(const hls_playlist_t *media_playlist)
{
    hls_track_t *track = vlc_list_first_entry_or_null(
        &media_playlist->tracks, hls_track_t, node);
    assert(track != NULL);
    return track;
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

static bool isTrackSupported(const es_format_t *fmt)
{
    return fmt->i_codec == VLC_CODEC_H264 || fmt->i_codec == VLC_CODEC_MP4A;
}

static int FormatCodecInfo(struct vlc_memstream *ms, const es_format_t *fmt)
{

    int wrote;
    switch (fmt->i_codec)
    {
        case VLC_CODEC_H264:
        {
            /* Parse the h264 constraint flag. */
            uint8_t constraints = 0;
            struct hxxx_helper hh;
            hxxx_helper_init(&hh, NULL, fmt->i_codec, 0, 0);
            if (hxxx_helper_set_extra(&hh, fmt->p_extra, fmt->i_extra) ==
                VLC_SUCCESS)
                h264_helper_get_constraint_flag(&hh, &constraints);
            hxxx_helper_clean(&hh);

            wrote = vlc_memstream_printf(ms,
                                         "avc1.%02X%02X%02X",
                                         fmt->i_profile,
                                         constraints,
                                         fmt->i_level);
            break;
        }
        case VLC_CODEC_MP4A:
            wrote = vlc_memstream_printf(
                ms,
                "mp4a.40.%02u",
                (fmt->i_profile == -1) ? 2u : fmt->i_profile + 1u);
            break;
        default:
            return VLC_ENOTSUP;
    }
    return (wrote == -1) ? VLC_ENOMEM : VLC_SUCCESS;
}

typedef struct VLC_VECTOR(const es_format_t *) es_format_vec_t;

static inline bool IsCodecAlreadyDescribed(const es_format_vec_t *vec,
                                           const es_format_t *fmt)
{
    const es_format_t *it;
    vlc_vector_foreach (it, vec)
    {
        if (es_format_IsSimilar(it, (fmt)))
            return true;
    }
    return false;
}

static char *GeneratePlaylistCodecInfo(const struct vlc_list *media_list,
                                       const hls_playlist_t *playlist)
{
    es_format_vec_t already_described = VLC_VECTOR_INITIALIZER;

    bool is_stream_empty = true;
    struct vlc_memstream out;
    vlc_memstream_open(&out);

    /* Describe codecs from the playlist. */
    const hls_track_t *track;
    vlc_list_foreach (track, &playlist->tracks, node)
    {
        if (IsCodecAlreadyDescribed(&already_described, &track->input->fmt))
            continue;

        if (!is_stream_empty)
            vlc_memstream_putc(&out, ',');
        if (FormatCodecInfo(&out, &track->input->fmt) != VLC_SUCCESS)
            goto error;
        is_stream_empty = false;
        vlc_vector_push(&already_described, &track->input->fmt);
    }

    /* Describe codecs from all the EXT-X-MEDIA tracks. */
    hls_playlist_t *media;
    vlc_list_foreach (media, media_list, node)
    {
        track = MediaGetTrack(media);

        if (IsCodecAlreadyDescribed(&already_described, &track->input->fmt))
            continue;

        if (!is_stream_empty)
            vlc_memstream_putc(&out, ',');
        if (FormatCodecInfo(&out, &track->input->fmt) != VLC_SUCCESS)
            goto error;
        is_stream_empty = false;
        vlc_vector_push(&already_described, &track->input->fmt);
    }

    vlc_vector_destroy(&already_described);

    vlc_memstream_putc(&out, '\0');
    if (vlc_memstream_close(&out) != 0)
        return NULL;
    return out.ptr;
error:
    if (vlc_memstream_close(&out) != 0)
        return NULL;
    free(out.ptr);
    vlc_vector_destroy(&already_described);
    return NULL;
}

static struct hls_storage *GenerateMainManifest(const sout_stream_sys_t *sys)
{
    struct vlc_memstream out;
    vlc_memstream_open(&out);

#define MANIFEST_START_TAG(tag)                                                \
    do                                                                         \
    {                                                                          \
        bool first_attribute = true;                                           \
        vlc_memstream_puts(&out, tag);

#define MANIFEST_ADD_ATTRIBUTE(attribute, ...)                                 \
    do                                                                         \
    {                                                                          \
        if (vlc_memstream_printf(&out,                                         \
                                 "%s" attribute,                               \
                                 first_attribute ? ":" : ",",                  \
                                 ##__VA_ARGS__) < 0)                           \
            goto error;                                                        \
        first_attribute = false;                                               \
    } while (0)

#define MANIFEST_END_TAG                                                       \
    vlc_memstream_putc(&out, '\n');                                            \
    }                                                                          \
    while (0)                                                                  \
        ;

    vlc_memstream_puts(&out, "#EXTM3U\n");

    static const char *const TRACK_TYPES[] = {
        [VIDEO_ES] = "VIDEO",
        [AUDIO_ES] = "AUDIO",
    };
    static const char *const GROUP_IDS[] = {
        [VIDEO_ES] = "video",
        [AUDIO_ES] = "audio",
    };

    const hls_playlist_t *playlist;
    vlc_list_foreach (playlist, &sys->media_playlists, node)
    {
        const hls_track_t *track = MediaGetTrack(playlist);
        const es_format_t *fmt = &track->input->fmt;
        assert(fmt->i_cat == VIDEO_ES || fmt->i_cat == AUDIO_ES);

        MANIFEST_START_TAG("#EXT-X-MEDIA")

            const char *track_type = TRACK_TYPES[fmt->i_cat];
            MANIFEST_ADD_ATTRIBUTE("TYPE=%s", track_type);

            const char *group_id = GROUP_IDS[fmt->i_cat];
            MANIFEST_ADD_ATTRIBUTE("GROUP-ID=\"%s\"", group_id);

            MANIFEST_ADD_ATTRIBUTE("NAME=\"%d\"", fmt->i_id);

            if (fmt->psz_language != NULL)
                MANIFEST_ADD_ATTRIBUTE("LANGUAGE=\"%s\"", fmt->psz_language);

            MANIFEST_ADD_ATTRIBUTE("URI=\"%s\"", playlist->url);

        MANIFEST_END_TAG
    }

    /* Format EXT-X-STREAM-INF */
    vlc_list_foreach (playlist, &sys->stream_inf_playlists, node)
    {
        MANIFEST_START_TAG("#EXT-X-STREAM-INF")

            unsigned int bandwidth = 0;
            const hls_track_t *track;
            vlc_list_foreach (track, &playlist->tracks, node)
                bandwidth += track->input->fmt.i_bitrate;
            MANIFEST_ADD_ATTRIBUTE("BANDWIDTH=%u", bandwidth);

            char *codecs =
                GeneratePlaylistCodecInfo(&sys->media_playlists, playlist);
            if (unlikely(codecs == NULL))
                goto error;
            MANIFEST_ADD_ATTRIBUTE("CODECS=\"%s\"", codecs);
            free(codecs);

            MANIFEST_ADD_ATTRIBUTE("VIDEO=\"%s\"", GROUP_IDS[VIDEO_ES]);
            MANIFEST_ADD_ATTRIBUTE("AUDIO=\"%s\"", GROUP_IDS[AUDIO_ES]);

        MANIFEST_END_TAG

        if (vlc_memstream_printf(&out, "%s\n", playlist->url) < 0)
            goto error;
    }

#undef MANIFEST_START_TAG
#undef MANIFEST_ADD_ATTRIBUTE
#undef MANIFEST_END_TAG

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

static ssize_t AccessOutWrite(sout_access_out_t *access, block_t *block)
{
    hls_playlist_t *playlist = access->p_sys;

    ssize_t wrote = 0;
    while (block != NULL)
    {
        if (playlist->ongoing_segment_data.length + block->i_length >=
            playlist->config->segment_length)
        {
            const int status = hls_segment_queue_NewSegment(
                &playlist->segments,
                playlist->ongoing_segment_data.begin,
                playlist->ongoing_segment_data.length);
            if (unlikely(status != VLC_SUCCESS))
                goto error;

            struct hls_storage *new_manifest =
                GeneratePlaylistManifest(playlist);
            if (unlikely(new_manifest == NULL))
                goto error;

            httpd_UrlCatch(playlist->http_manifest,
                           HTTPD_MSG_GET,
                           HTTPCallback,
                           (httpd_callback_sys_t *)new_manifest);

            if (playlist->manifest != NULL)
                hls_storage_Destroy(playlist->manifest);
            playlist->manifest = new_manifest;

            ResetSegmentBuffer(&playlist->ongoing_segment_data);
        }

        wrote += block->i_buffer;

        block_t *next = block->p_next;

        block->p_next = NULL;
        playlist->ongoing_segment_data.length += block->i_length;
        block_ChainLastAppend(&playlist->ongoing_segment_data.end, block);

        block = next;
    }

    return wrote;
error:
    block_ChainRelease(block);
    return -1;
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

static inline char *FormatPlaylistManifestURL(const hls_playlist_t *playlist)
{
    char *url;
    const int status = asprintf(
        &url, "%s/playlist-%u-index.m3u8", playlist->config->base_url, playlist->id);
    if (unlikely(status == -1))
        return NULL;
    return url;
}

static hls_playlist_t *CreatePlaylist(sout_stream_t *stream)
{
    sout_stream_sys_t *sys = stream->p_sys;

    hls_playlist_t *playlist = calloc(1, sizeof(*playlist));
    if (unlikely(playlist == NULL))
        return NULL;

    playlist->access = CreateAccessOut(stream, playlist);
    if (unlikely(playlist->access == NULL))
        goto error;

    playlist->mux = sout_MuxNew(playlist->access, "ts");
    if (unlikely(playlist->mux == NULL))
        goto error;

    playlist->id = sys->playlist_created_count;
    playlist->config = &sys->config;

    playlist->url = FormatPlaylistManifestURL(playlist);
    if (unlikely(playlist->url == NULL))
        goto error;

    hls_segment_queue_Init(
        &playlist->segments, playlist->id, sys->http_host, &sys->config);

    ResetSegmentBuffer(&playlist->ongoing_segment_data);

    playlist->manifest = NULL;
    playlist->http_manifest =
        httpd_UrlNew(sys->http_host, playlist->url, NULL, NULL);
    if (playlist->http_manifest == NULL)
        goto error;

    vlc_list_init(&playlist->tracks);

    return playlist;
error:
    if (playlist->access != NULL)
        sout_AccessOutDelete(playlist->access);
    if (playlist->mux != NULL)
        sout_MuxDelete(playlist->mux);
    free(playlist->url);
    free(playlist);
    return NULL;
}

static hls_playlist_t* NewStreamInf(sout_stream_t *stream)
{
    sout_stream_sys_t *sys = stream->p_sys;

    hls_playlist_t *stream_inf = CreatePlaylist(stream);
    if(stream_inf != NULL)
        vlc_list_append(&stream_inf->node, &sys->stream_inf_playlists);
    return stream_inf;
}

static hls_playlist_t* NewMedia(sout_stream_t *stream)
{
    sout_stream_sys_t *sys = stream->p_sys;

    hls_playlist_t *media = CreatePlaylist(stream);
    if(media != NULL)
        vlc_list_append(&media->node, &sys->media_playlists);
    return media;
}

static void DeletePlaylist(hls_playlist_t *playlist)
{
    sout_MuxDelete(playlist->mux);

    sout_AccessOutDelete(playlist->access);

    httpd_UrlDelete(playlist->http_manifest);

    if (playlist->manifest != NULL)
        hls_storage_Destroy(playlist->manifest);

    block_ChainRelease(playlist->ongoing_segment_data.begin);
    hls_segment_queue_Clear(&playlist->segments);

    vlc_list_remove(&playlist->node);

    free(playlist->url);

    free(playlist);
}

static struct stream_inf_desc *DescInfFromID(sout_stream_sys_t *sys,
                                          unsigned int id)
{

    struct stream_inf_desc *it;
    vlc_vector_foreach (it, &sys->stream_inf_descriptors)
    {
        unsigned int es_id;
        vlc_vector_foreach (es_id, &it->es_list)
        {
            if (es_id == id)
                return it;
        }
    }
    return NULL;
}

static struct stream_inf_desc *DescFromPlaylist(sout_stream_sys_t *sys,
                                                hls_playlist_t *playlist)
{
    struct stream_inf_desc *it;
    vlc_vector_foreach (it, &sys->stream_inf_descriptors)
    {
        if (it->playlist_ref == playlist)
            return it;
    }
    return NULL;
}

static void *Add(sout_stream_t *stream, const es_format_t *fmt)
{

    if (!isTrackSupported(fmt))
        return NULL;

    sout_stream_sys_t *sys = stream->p_sys;

    // Either retrieve the already created playlist in StreamInf or create it.
    struct stream_inf_desc *desc = DescInfFromID(sys, fmt->i_id);
    hls_playlist_t *playlist;
    if (desc != NULL)
    {
        if (desc->playlist_ref == NULL)
            desc->playlist_ref = NewStreamInf(stream);
        playlist = desc->playlist_ref;
    }
    else
        playlist = NewMedia(stream);

    if (playlist == NULL)
        return NULL;

    ++sys->playlist_created_count;

    sout_input_t *input = sout_MuxAddStream(playlist->mux, fmt);
    if (input == NULL)
        goto error;

    hls_track_t *track = malloc(sizeof(*track));
    if (unlikely(track == NULL))
        goto error;

    track->input = input;
    track->playlist_ref = playlist;

    vlc_list_append(&track->node, &playlist->tracks);

    struct hls_storage *new_manifest = GenerateMainManifest(sys);
    if (unlikely(new_manifest == NULL))
    {
        vlc_list_remove(&track->node);
        free(track);
        goto error;
    }

    httpd_UrlCatch(sys->http_manifest,
                   HTTPD_MSG_GET,
                   HTTPCallback,
                   (httpd_callback_sys_t *)new_manifest);

    if (sys->manifest != NULL)
        hls_storage_Destroy(sys->manifest);
    sys->manifest = new_manifest;

    if (desc != NULL && desc->playlist_ref == NULL)
        desc->playlist_ref = playlist;

    return track;
error:
    if (input != NULL)
        sout_MuxDeleteStream(playlist->mux, input);
    if (vlc_list_is_empty(&playlist->tracks))
        DeletePlaylist(playlist);
    return NULL;
}

static void Del(sout_stream_t *stream, void *id)
{
    sout_stream_sys_t *sys = stream->p_sys;
    hls_track_t *track = id;

    sout_MuxDeleteStream(track->playlist_ref->mux, track->input);
    vlc_list_remove(&track->node);

    if (vlc_list_is_empty(&track->playlist_ref->tracks))
    {
        struct stream_inf_desc *stream_inf =
            DescFromPlaylist(sys, track->playlist_ref);
        if (stream_inf != NULL)
            stream_inf->playlist_ref = NULL;

        DeletePlaylist(track->playlist_ref);
    }

    free(track);
}

static int Send(sout_stream_t *stream, void *id, vlc_frame_t *frame)
{
    hls_track_t *track = id;
    return sout_MuxSendBuffer(track->playlist_ref->mux, track->input, frame);
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
        goto error;

    sys->http_manifest =
        httpd_UrlNew(sys->http_host, "/stream.m3u8", NULL, NULL);
    if (sys->http_manifest == NULL)
    {
        httpd_HostDelete(sys->http_host);
        goto error;
    }
    sys->manifest = NULL;

    sys->config.base_url = var_GetString(stream, SOUT_CFG_PREFIX "base-url");
    sys->config.max_segments =
        var_GetInteger(stream, SOUT_CFG_PREFIX "numsegs");
    sys->config.pace = var_GetBool(stream, SOUT_CFG_PREFIX "pace");
    sys->config.segment_length =
        VLC_TICK_FROM_SEC(var_GetInteger(stream, SOUT_CFG_PREFIX "seglen"));

    vlc_vector_init(&sys->stream_inf_descriptors);
    struct stream_inf_desc *inf = calloc(1, sizeof(*inf));
    vlc_vector_init(&inf->es_list);
    vlc_vector_push(&inf->es_list, 1);
    //vlc_vector_push(&inf->es_list, 2);
    vlc_vector_push(&sys->stream_inf_descriptors, inf);
    sys->playlist_created_count = 0;

    vlc_list_init(&sys->stream_inf_playlists);
    vlc_list_init(&sys->media_playlists);

    static const struct sout_stream_operations ops = {
        .add = Add,
        .del = Del,
        .send = Send,
        .control = Control,
    };
    stream->ops = &ops;
    stream->p_sys = sys;

    return VLC_SUCCESS;
error:
    free(sys);
    return VLC_EGENERIC;
}

static void Close(vlc_object_t *this)
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;

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
