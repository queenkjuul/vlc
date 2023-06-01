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
#include <vlc_configuration.h>
#include <vlc_frame.h>
#include <vlc_list.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_tick.h>
#include <vlc_vector.h>

#include "hls.h"
#include "segments.h"

typedef struct
{
    block_t *begin;
    block_t **end;
    vlc_tick_t length;
} hls_block_chain_t;

static inline void hls_block_chain_Reset(hls_block_chain_t *chain)
{
    chain->begin = NULL;
    chain->end = &chain->begin;
    chain->length = 0;
}

/**
 * Represent one HLS playlist as in RFC 8216 section 4.
 */
typedef struct
{
    unsigned int id;

    sout_access_out_t *access;
    sout_mux_t *mux;
    /** Every ES muxed in this playlist. */
    struct vlc_list tracks;

    hls_block_chain_t muxed_output;

    /**
     * Completed segments queue.
     *
     * The queue is generally max-sized (configurable by the user). Which means
     * that, when the max size is reached, pushing in the queue erase the first
     * segment.
     */
    hls_segment_queue_t segments;

    vlc_tick_t first_output_dts;

    struct vlc_list node;
} hls_playlist_t;

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

    vlc_tick_t first_pcr;
    vlc_tick_t last_segment;
} sout_stream_sys_t;

static ssize_t AccessOutWrite(sout_access_out_t *access, block_t *block)
{
    hls_playlist_t *playlist = access->p_sys;

    if (playlist->first_output_dts == VLC_TICK_INVALID)
        playlist->first_output_dts =
            (block->i_dts != VLC_TICK_INVALID) ? block->i_dts : block->i_pts;

    size_t wrote = 0;
    block_ChainProperties(block, NULL, &wrote, NULL);
    block_ChainLastAppend(&playlist->muxed_output.end, block);
    return wrote;
}

static sout_access_out_t *CreateAccessOut(sout_stream_t *stream,
                                          hls_playlist_t *sys)
{
    sout_access_out_t *access = vlc_object_create(stream, sizeof(*access));
    if (unlikely(access == NULL))
        return NULL;

    access->psz_access = strdup("hls");
    if (unlikely(access->psz_access == NULL))
    {
        vlc_object_delete(access);
        return NULL;
    }

    access->p_cfg = NULL;
    access->p_module = NULL;
    access->p_sys = sys;
    access->psz_path = NULL;

    access->pf_control = NULL;
    access->pf_read = NULL;
    access->pf_seek = NULL;
    access->pf_write = AccessOutWrite;
    return access;
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
    playlist->first_output_dts = VLC_TICK_INVALID;

    struct hls_segment_queue_config config = {
        .playlist_id = playlist->id,
    };
    hls_segment_queue_Init(&playlist->segments, &config, &sys->config);

    hls_block_chain_Reset(&playlist->muxed_output);

    vlc_list_init(&playlist->tracks);

    return playlist;
error:
    if (playlist->access != NULL)
        sout_AccessOutDelete(playlist->access);
    if (playlist->mux != NULL)
        sout_MuxDelete(playlist->mux);
    free(playlist);
    return NULL;
}

static hls_playlist_t *NewStreamInf(sout_stream_t *stream)
{
    sout_stream_sys_t *sys = stream->p_sys;

    hls_playlist_t *stream_inf = CreatePlaylist(stream);
    if (stream_inf != NULL)
        vlc_list_append(&stream_inf->node, &sys->stream_inf_playlists);
    return stream_inf;
}

static hls_playlist_t *NewMedia(sout_stream_t *stream)
{
    sout_stream_sys_t *sys = stream->p_sys;

    hls_playlist_t *media = CreatePlaylist(stream);
    if (media != NULL)
        vlc_list_append(&media->node, &sys->media_playlists);
    return media;
}

static void DeletePlaylist(hls_playlist_t *playlist)
{
    sout_MuxDelete(playlist->mux);

    sout_AccessOutDelete(playlist->access);

    block_ChainRelease(playlist->muxed_output.begin);
    hls_segment_queue_Clear(&playlist->segments);

    vlc_list_remove(&playlist->node);

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

static hls_block_chain_t ExtractSegment(hls_playlist_t *playlist,
                                        vlc_tick_t last_segment_time,
                                        vlc_tick_t max_segment_length)
{
    hls_block_chain_t segment = {.begin = playlist->muxed_output.begin};

    block_t *prev = NULL;
    for (block_t *it = playlist->muxed_output.begin; it != NULL;
         it = it->p_next)
    {
        const vlc_tick_t block_end_time =
            (it->i_dts + it->i_length) - playlist->first_output_dts;
        if (block_end_time - last_segment_time > max_segment_length)
        {
            playlist->muxed_output.begin = it;

            if (prev != NULL)
                prev->p_next = NULL;
            return segment;
        }
        segment.length = block_end_time - last_segment_time;
        prev = it;
    }

    hls_block_chain_Reset(&playlist->muxed_output);
    return segment;
}

static void ExtractAndAddSegment(hls_playlist_t *playlist,
                                 vlc_tick_t last_segment_time,
                                 vlc_tick_t max_segment_length)
{
    hls_block_chain_t segment =
        ExtractSegment(playlist, last_segment_time, max_segment_length);

    const int status = hls_segment_queue_NewSegment(
        &playlist->segments, segment.begin, segment.length);
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

        hls_playlist_t *playlist;
        vlc_list_foreach (playlist, &sys->stream_inf_playlists, node)
            ExtractAndAddSegment(
                playlist, sys->last_segment, sys->config.segment_length);
        vlc_list_foreach (playlist, &sys->media_playlists, node)
            ExtractAndAddSegment(
                playlist, sys->last_segment, sys->config.segment_length);

        DeletePlaylist(track->playlist_ref);
    }

    free(track);
}

static int Send(sout_stream_t *stream, void *id, vlc_frame_t *frame)
{
    hls_track_t *track = id;
    return sout_MuxSendBuffer(track->playlist_ref->mux, track->input, frame);
    (void)stream;
}

/**
 * PCR events are used to have a reliable stream time status. Segmenting is done
 * after a PCR testifying that we are above the segment limit arrives.
 */
static void SetPCR(sout_stream_t *stream, vlc_tick_t pcr)
{
    sout_stream_sys_t *sys = stream->p_sys;

    if (sys->first_pcr == VLC_TICK_INVALID)
    {
        sys->first_pcr = pcr;
        return;
    }

    const vlc_tick_t stream_time = pcr - sys->first_pcr;
    if (stream_time - sys->last_segment > sys->config.segment_length)
    {
        hls_playlist_t *playlist;
        vlc_list_foreach (playlist, &sys->stream_inf_playlists, node)
            ExtractAndAddSegment(
                playlist, sys->last_segment, sys->config.segment_length);
        vlc_list_foreach (playlist, &sys->media_playlists, node)
            ExtractAndAddSegment(
                playlist, sys->last_segment, sys->config.segment_length);
        sys->last_segment = stream_time;
    }
}

#define SOUT_CFG_PREFIX "sout-hls-"

static int Open(vlc_object_t *this)
{
    sout_stream_t *stream = (sout_stream_t *)this;

    sout_stream_sys_t *sys = malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;
    stream->p_sys = sys;

    static const char *const options[] = {
        "base-url", "num-seg", "out-dir", "pace", "seg-len", NULL};
    config_ChainParse(stream, SOUT_CFG_PREFIX, options, stream->p_cfg);

    sys->config.base_url = var_GetString(stream, SOUT_CFG_PREFIX "base-url");
    sys->config.outdir =
        var_GetNonEmptyString(stream, SOUT_CFG_PREFIX "out-dir");
    sys->config.max_segments =
        var_GetInteger(stream, SOUT_CFG_PREFIX "num-seg");
    sys->config.pace = var_GetBool(stream, SOUT_CFG_PREFIX "pace");
    sys->config.segment_length =
        VLC_TICK_FROM_SEC(var_GetInteger(stream, SOUT_CFG_PREFIX "seg-len"));

    vlc_vector_init(&sys->stream_inf_descriptors);

    /* FIXME: This is highly temporary work.
     *
     * Force create the playlist representation by feeding some standard ES id.
     * This will obviously be removed once parsing from and user option is
     * implemented.
     */
    struct stream_inf_desc *inf = calloc(1, sizeof(*inf));
    vlc_vector_init(&inf->es_list);
    vlc_vector_push(&inf->es_list, 1);
    vlc_vector_push(&inf->es_list, 2);
    vlc_vector_push(&sys->stream_inf_descriptors, inf);

    sys->playlist_created_count = 0;

    vlc_list_init(&sys->stream_inf_playlists);
    vlc_list_init(&sys->media_playlists);

    sys->first_pcr = VLC_TICK_INVALID;
    sys->last_segment = 0;

    static const struct sout_stream_operations ops = {
        .add = Add,
        .del = Del,
        .send = Send,
        .set_pcr = SetPCR,
    };
    stream->ops = &ops;

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *this)
{
    sout_stream_t *stream = (sout_stream_t *)this;
    sout_stream_sys_t *sys = stream->p_sys;

    hls_config_Clean(&sys->config);

    free(sys);
}

#define BASEURL_TEXT N_("Base of the URL")
#define NUMSEG_TEXT N_("Number of maximum segment exposed")
#define OUTDIR_TEXT N_("Output directory path")
#define OUTDIR_LONGTEXT                                                        \
    N_("Output directory path. If not specified and HTTP is enabled, the "     \
       "segments will be stored in memory.")
#define PACE_LONGTEXT                                                          \
    N_("Enable input pacing, the media will play at a playbak rate")
#define PACE_TEXT N_("Enable pacing")
#define SEGLEN_LONGTEXT N_("Length of segments in seconds")
#define SEGLEN_TEXT N_("Segment length (sec)")

vlc_module_begin()
    set_shortname("HLS")
    set_description(N_("HLS stream output"))
    set_capability("sout output", 50)
    add_shortcut("hls")
    set_subcategory(SUBCAT_SOUT_STREAM)

    add_string(SOUT_CFG_PREFIX "base-url", "", BASEURL_TEXT, BASEURL_TEXT)
    add_integer(SOUT_CFG_PREFIX "num-seg", 0, NUMSEG_TEXT, NUMSEG_TEXT)
    add_string(SOUT_CFG_PREFIX "out-dir", NULL, OUTDIR_TEXT, OUTDIR_LONGTEXT)
    add_bool(SOUT_CFG_PREFIX "pace", false, PACE_TEXT, PACE_LONGTEXT)
    add_integer(SOUT_CFG_PREFIX "seg-len", 4, SEGLEN_TEXT, SEGLEN_LONGTEXT)

    set_callbacks(Open, Close)
vlc_module_end()
