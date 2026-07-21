/*****************************************************************************
 * cast.cpp: Chromecast sout module for vlc
 *****************************************************************************
 * Copyright © 2014-2015 VideoLAN
 *
 * Authors: Adrien Maglo <magsoft@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Steve Lhomme <robux4@videolabs.io>
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

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "../renderer_common.hpp"
#include "chromecast.h"
#include "cc_startup_cache.h"
#include "../../packetizer/flac.h" /* STREAMINFO parsing for the FLAC depth gate */
#include <vlc_configuration.h>
#include <vlc_dialog.h>

#include <vlc_sout.h>
#include <vlc_block.h>
#include <vlc_modules.h>
#include <vlc_httpd.h>

#include <cassert>

#define TRANSCODING_NONE  0x0
#define TRANSCODING_VIDEO 0x1
#define TRANSCODING_AUDIO 0x2

#if 0
/* TODO: works only with internal spu and transcoding/blending for now */
#define CC_ENABLE_SPU
#endif

namespace {

struct sout_stream_sys_t
{
    sout_stream_sys_t(httpd_host_t *httpd_host, intf_sys_t * const intf,
                      bool has_video, int port)
        : httpd_host(httpd_host)
        , p_cache(nullptr)
        , p_intf(intf)
        , b_supports_video(has_video)
        , i_port(port)
        , cc_has_input(false)
        , cc_flushing(false)
        , cc_eof(false)
        , perf_warning_shown(false)
        , transcoding_state(TRANSCODING_NONE)
        , venc_opt_idx(-1)
    {
        assert(p_intf != nullptr);
        vlc_mutex_init(&lock);
    }

    ~sout_stream_sys_t() = default;

    bool canDecodeVideo(const es_format_t *) const;
    bool canDecodeAudio(sout_stream_t *, const es_format_t *,
                        bool b_has_video) const;
    bool transcodingCanFallback() const;
    void setNextTranscodingState();
    std::string GetAcodecOption(sout_stream_t *, vlc_fourcc_t *,
                                const audio_format_t *, int);

    httpd_host_t    *httpd_host;
    sout_stream_t   *p_cache;   /* startup-cache helper; always valid after Open */
    std::string      mime;
    /* The LOAD mime is always the HLS manifest type, so it can no longer tell
     * the receiver whether this is a music cast. Tracked explicitly instead. */
    bool             audio_only = false;
    vlc_mutex_t      lock;      /* guards all mutable state; taken by input events too */
    intf_sys_t      * const p_intf;
    const bool       b_supports_video;
    const int        i_port;
    bool             cc_has_input;
    bool             cc_flushing;
    bool             cc_eof;
    bool             perf_warning_shown;
    int              transcoding_state;
    int              venc_opt_idx;
};

} // namespace

#define SOUT_CFG_PREFIX "sout-chromecast-"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  Open(vlc_object_t *);
static void Close(sout_stream_t *);
static bool BuildHLSChain(void *opaque, sout_stream_t *cache,
                          cc_startup_cache_sys_t *csys);

static const char *const ppsz_sout_options[] = {
    "ip", "port", "http-port", "video", "device-name", NULL
};

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

#define HTTP_PORT_TEXT N_("HTTP port")
#define HTTP_PORT_LONGTEXT N_("This sets the HTTP port of the local server " \
                              "used to stream the media to the Chromecast.")

#define IP_ADDR_TEXT N_("IP Address")
#define IP_ADDR_LONGTEXT N_("IP Address of the Chromecast.")
#define PORT_TEXT N_("Chromecast port")
#define PORT_LONGTEXT N_("The port used to talk to the Chromecast.")


vlc_module_begin ()

    set_shortname(N_("Chromecast"))
    set_description(N_("Chromecast stream output"))
    set_capability("sout output", 0)
    add_shortcut("chromecast")
    set_subcategory(SUBCAT_SOUT_STREAM)
    set_callback(Open)

    add_string(SOUT_CFG_PREFIX "ip", NULL, NULL, NULL)
        change_private()
    add_integer(SOUT_CFG_PREFIX "port", CHROMECAST_CONTROL_PORT, NULL, NULL)
        change_private()
    add_string(SOUT_CFG_PREFIX "device-name", NULL, NULL, NULL)
        change_private()
    add_bool(SOUT_CFG_PREFIX "video", true, NULL, NULL)
        change_private()
    add_integer(SOUT_CFG_PREFIX "http-port", HTTP_PORT, HTTP_PORT_TEXT, HTTP_PORT_LONGTEXT)
    add_obsolete_string(SOUT_CFG_PREFIX "mux") /* since 4.0.0 */
    add_obsolete_string(SOUT_CFG_PREFIX "mime") /* since 4.0.0 */
    add_renderer_opts(SOUT_CFG_PREFIX)
vlc_module_end ()

/*****************************************************************************
 * Codec capability helpers
 *
 * These describe what the device accepts *through the HLS/fMP4 output*, which
 * is narrower than what it can decode from WebM/Matroska. Everything accepted
 * here must also pass hls_codec_IsSupported() in
 * modules/stream_out/hls/codecs.c - keep the two in step.
 *****************************************************************************/

bool sout_stream_sys_t::canDecodeVideo( const es_format_t *es ) const
{
    if( transcoding_state & TRANSCODING_VIDEO )
        return false;

    const std::string suffix = "(Chromecast)";
    const std::string name = p_intf->getDeviceName();
    const bool original_chromecast = name.size() >= suffix.size() &&
        std::equal(suffix.rbegin(), suffix.rend(), name.rbegin());

    switch( es->i_codec )
    {
        case VLC_CODEC_HEVC:
            if (original_chromecast)
                return false;
            return true;
        case VLC_CODEC_H264:
            return true;
        /* VP8/VP9 are decodable by Cast devices from WebM, but have no usable
         * mapping into fMP4 segments (VLC cannot even mux them), and AV1 was
         * tested and rejected by the receiver's HLS pipeline. All transcode. */
        default:
            return false;
    }
}

/* The receiver refuses 24-bit FLAC. audio.i_bitspersample is only filled in by
 * some demuxers (matroska sets it from BitDepth; the FLAC packetizer parses the
 * depth but does not publish it), so fall back to the STREAMINFO block carried
 * in the extradata. When the depth cannot be determined at all, attempt
 * passthrough anyway: a refusal escalates through the retry ladder to AAC,
 * which is preferable to needlessly transcoding every FLAC whose depth we
 * merely failed to read. */
static bool FlacBitDepthIsCastable( const es_format_t *p_es )
{
    if( p_es->audio.i_bitspersample != 0 )
        return p_es->audio.i_bitspersample <= 16;

    const uint8_t *p_extra = static_cast<const uint8_t *>( p_es->p_extra );
    size_t i_extra = p_es->i_extra;
    if( p_extra == NULL )
        return true;

    /* Native extradata is "fLaC" + METADATA_BLOCK header + STREAMINFO; the
     * packetizer hands out a bare STREAMINFO (modules/packetizer/flac.c:118). */
    if( i_extra > 8 && !memcmp( p_extra, "fLaC", 4 ) )
    {
        p_extra += 8;
        i_extra -= 8;
    }
    if( i_extra < FLAC_STREAMINFO_SIZE )
        return true;

    struct flac_stream_info stream_info;
    FLAC_ParseStreamInfo( p_extra, &stream_info );
    return stream_info.bits_per_sample <= 16;
}

bool sout_stream_sys_t::canDecodeAudio( sout_stream_t *p_stream,
                                        const es_format_t *p_es,
                                        bool b_has_video ) const
{
    if( transcoding_state & TRANSCODING_AUDIO )
        return false;
    switch( p_es->i_codec )
    {
        case VLC_CODEC_A52:
        case VLC_CODEC_EAC3:
            return var_InheritBool( p_stream, SOUT_CFG_PREFIX "audio-passthrough" );
        case VLC_FOURCC('h', 'a', 'a', 'c'):
        case VLC_FOURCC('l', 'a', 'a', 'c'):
        case VLC_FOURCC('s', 'a', 'a', 'c'):
        case VLC_CODEC_MP4A:
            return p_es->audio.i_channels <= 2;
        /* Opus and FLAC play back only in audio-only casts: alongside a video
         * track the receiver rejects the LOAD outright (tested; cause not yet
         * identified, so passthrough is limited to the shape known to work). */
        case VLC_CODEC_OPUS:
            return !b_has_video;
        case VLC_CODEC_FLAC:
            return !b_has_video && FlacBitDepthIsCastable( p_es );
        /* Vorbis has no ISOBMFF mapping at all; MP3/MP2/MPGA are unreliable
         * under MSE as mp4a.40.34. All transcode to AAC. */
        default:
            return false;
    }
}

std::string
sout_stream_sys_t::GetAcodecOption( sout_stream_t *p_stream,
                                    vlc_fourcc_t *p_codec_audio,
                                    const audio_format_t *p_aud, int i_quality )
{
    VLC_UNUSED( p_aud );
    VLC_UNUSED( i_quality );
    std::stringstream ssout;

    /* AAC is the only audio codec the receiver accepts in every cast shape, so
     * it is the sole transcoding target - the quality setting no longer picks
     * between codecs, only the encoder's own bitrate matters. */
    *p_codec_audio = VLC_CODEC_MP4A;

    msg_Dbg( p_stream, "Converting audio to %4.4s", (const char*)p_codec_audio );

    char fourcc[5];
    vlc_fourcc_to_char( *p_codec_audio, fourcc );
    fourcc[4] = '\0';
    ssout << "acodec=" << fourcc << ',';
    /* Cast AAC decoding is stereo only (cf. the i_channels <= 2 remux gate in
     * canDecodeAudio), so downmix explicitly rather than handing the encoder a
     * multichannel input it cannot signal. */
    ssout << "ab=" << CC_AUDIO_BITRATE_KBPS << ",channels=2,";
    return ssout.str();
}

bool sout_stream_sys_t::transcodingCanFallback() const
{
    return transcoding_state != (TRANSCODING_VIDEO | TRANSCODING_AUDIO);
}

void sout_stream_sys_t::setNextTranscodingState()
{
    if (!(transcoding_state & TRANSCODING_VIDEO))
        transcoding_state |= TRANSCODING_VIDEO;
    else if (!(transcoding_state & TRANSCODING_AUDIO))
        /* |=, not =: dropping the VIDEO bit here would leave
         * transcodingCanFallback() permanently true and cycle the ladder
         * forever instead of terminating at VIDEO|AUDIO. */
        transcoding_state |= TRANSCODING_AUDIO;
}

/*****************************************************************************
 * HLS readiness callback — fired by the hls module once enough segments exist
 *****************************************************************************/

static int OnHlsReady(vlc_object_t *, const char *, vlc_value_t,
                      vlc_value_t newval, void *data)
{
    sout_stream_sys_t *p_sys = static_cast<sout_stream_sys_t *>(data);
    if (newval.b_bool && !p_sys->cc_has_input)
    {
        p_sys->p_intf->setHasInput(p_sys->mime, p_sys->audio_only);
        p_sys->cc_has_input = true;
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * BuildHLSChain — cc_cache_build_fn callback
 *
 * Called by the startup cache once CC_STARTUP_CACHING PCR has elapsed.
 * Selects one audio and one video track, builds the HLS sout chain with
 * the correct variants= list, wires the selected tracks into the chain,
 * and replays the buffered event FIFO.
 *****************************************************************************/

/* Release all buffered events and clear selections — shared failure cleanup. */
static void BuildFailCleanup(cc_startup_cache_sys_t *csys)
{
    for (auto *t : csys->tracks)
    {
        t->selected      = false;
        t->downstream_id = nullptr;
    }
    for (auto &ev : csys->events)
        if (!ev.is_pcr && ev.block)
            block_ChainRelease(ev.block);
    csys->events.clear();
}

static bool BuildHLSChain(void *opaque, sout_stream_t *cache,
                           cc_startup_cache_sys_t *csys)
{
    sout_stream_t     *p_stream = static_cast<sout_stream_t *>(opaque);
    sout_stream_sys_t *p_sys    =
        static_cast<sout_stream_sys_t *>(p_stream->p_sys);

    /* ---- ES selection ---- */
    bool canRemux = true;
    vlc_fourcc_t i_codec_video = 0, i_codec_audio = 0;
    const es_format_t *p_original_audio = nullptr;
    const es_format_t *p_original_video = nullptr;

    /* Whether a video track will be part of the cast has to be known before the
     * audio codec is probed: Opus and FLAC are only accepted for passthrough in
     * audio-only casts (see canDecodeAudio). */
    bool b_has_video = false;
    if (p_sys->b_supports_video)
    {
        for (const auto *track : csys->tracks)
        {
            if (track->fmt.i_cat == VIDEO_ES)
            {
                b_has_video = true;
                break;
            }
        }
    }

    for (auto *track : csys->tracks)
    {
        const es_format_t *p_es = &track->fmt;
        if (p_es->i_cat == AUDIO_ES && !p_original_audio)
        {
            if (!p_sys->canDecodeAudio(p_stream, p_es, b_has_video))
            {
                msg_Dbg(p_stream, "can't remux audio track %d codec %4.4s",
                        p_es->i_id, (const char*)&p_es->i_codec);
                canRemux = false;
            }
            else if (!i_codec_audio)
                i_codec_audio = p_es->i_codec;
            p_original_audio = p_es;
            track->selected  = true;
        }
        else if (p_sys->b_supports_video &&
                 p_es->i_cat == VIDEO_ES && !p_original_video)
        {
            if (!p_sys->canDecodeVideo(p_es))
            {
                msg_Dbg(p_stream, "can't remux video track %d codec %4.4s",
                        p_es->i_id, (const char*)&p_es->i_codec);
                canRemux = false;
            }
            else if (!i_codec_video)
                i_codec_video = p_es->i_codec;
            p_original_video = p_es;
            track->selected  = true;
        }
        else if (p_sys->b_supports_video && p_es->i_cat == SPU_ES)
        {
            track->selected  = true;
        }
    }

    if (!p_original_audio && !p_original_video)
    {
        BuildFailCleanup(csys);
        p_sys->p_intf->requestPlayerStop();
        return false;
    }

    /* ---- Optional transcode prefix ---- */
    std::stringstream ssout;
    int new_transcoding_state = TRANSCODING_NONE;

    if (!canRemux)
    {
        if (!p_sys->perf_warning_shown && !i_codec_video && p_original_video
            && var_InheritInteger(p_stream,
                                  RENDERER_CFG_PREFIX "show-perf-warning"))
        {
            int res = vlc_dialog_wait_question(p_stream,
                          VLC_DIALOG_QUESTION_WARNING,
                          _("Cancel"), _("OK"), _("OK, don't warn me again"),
                          _("Performance warning"),
                          _("Casting this video requires conversion. "
                            "This conversion can use all the available power and "
                            "could quickly drain your battery."));
            p_sys->perf_warning_shown = true;
            if (res <= 0)
            {
                BuildFailCleanup(csys);
                return false;
            }
            if (res == 2)
                config_PutInt(RENDERER_CFG_PREFIX "show-perf-warning", 0);
        }

        const int i_quality =
            var_InheritInteger(p_stream, SOUT_CFG_PREFIX "conversion-quality");

        ssout << "transcode{";
        if (!i_codec_audio && p_original_audio)
        {
            ssout << p_sys->GetAcodecOption(p_stream, &i_codec_audio,
                                            &p_original_audio->audio, i_quality);
            new_transcoding_state |= TRANSCODING_AUDIO;
        }
        if (!i_codec_video && p_original_video)
        {
            try {
                /* H.264 only: VP8 cannot be carried in fMP4 segments, so it is
                 * not a usable fallback here. GetVcodecOption live-probes the
                 * available encoders and throws when none work. */
                ssout << vlc_sout_renderer_GetVcodecOption(p_stream,
                            { VLC_CODEC_H264 },
                            &i_codec_video,
                            &p_original_video->video, i_quality);
                new_transcoding_state |= TRANSCODING_VIDEO;
            }
            catch (const std::exception &)
            {
                BuildFailCleanup(csys);
                return false;
            }
        }
        ssout << "}:";
    }

    p_sys->mime              = "application/vnd.apple.mpegurl";
    p_sys->audio_only        = (p_original_video == nullptr);
    p_sys->transcoding_state = new_transcoding_state;

    /* ---- Build the hls{} chain string ---- */
    /* For fMP4, Chromecast's HLS pipeline (Shaka/MSE) expects demuxed
     * renditions: a single multi-track fMP4 segment with audio+video muxed is
     * not accepted (MSE uses one SourceBuffer per content type). Put only the
     * video track in the variant map; the HLS module turns every ES left out
     * of the map into its own EXT-X-MEDIA rendition, giving audio its own
     * audio-only segments. With no video (audio-only cast) the audio track
     * becomes the variant instead. */
    const int variant_cat = (p_original_video != nullptr) ? VIDEO_ES : AUDIO_ES;
    ssout << "hls{variants={{";
    bool first = true;
    for (auto *track : csys->tracks)
    {
        if (!track->selected) continue;
        if (track->fmt.i_cat != variant_cat) continue;
        if (!first) ssout << ',';
        ssout << track->es_id;
        first = false;
    }
    ssout << "}},seg-type=fmp4,host-http"
          << ",seg-len=" << CC_HLS_SEG_LEN_SEC
          << ",max-seg-len=" << CC_HLS_MAX_SEG_LEN_SEC
          << ",num-seg=" << CC_HLS_NUM_SEG
          << ",base-url="
          << p_sys->p_intf->getHttpStreamRoot() << "}";

    const std::string chain_str = ssout.str();
    msg_Dbg(p_stream, "Creating chain %s", chain_str.c_str());

    sout_stream_t *p_out =
        sout_StreamChainNew(VLC_OBJECT(p_stream), chain_str.c_str(), nullptr);
    if (!p_out)
    {
        msg_Err(p_stream, "could not create sout chain: %s", chain_str.c_str());
        BuildFailCleanup(csys);
        p_sys->p_intf->requestPlayerStop();
        return false;
    }

    /* Register hls-ready readiness var callback and record it so the cache
     * can remove it cleanly on reset. */
    sout_stream_t *hls_out =
        (new_transcoding_state != TRANSCODING_NONE) ? p_out->p_next : p_out;

    var_AddCallback(hls_out, "hls-ready", OnHlsReady, p_sys);
    csys->hls_out        = hls_out;
    csys->hls_ready_fn   = OnHlsReady;
    csys->hls_ready_data = p_sys;

    /* ---- Wire selected tracks into the chain ---- */
    cache->p_next  = p_out;
    bool all_wired = true;
    for (auto *track : csys->tracks)
    {
        if (!track->selected) continue;
        track->downstream_id =
            sout_StreamIdAdd(p_out, &track->fmt, track->es_id);
        if (!track->downstream_id)
        {
            msg_Err(p_stream, "can't handle %4.4s stream",
                    (const char*)&track->fmt.i_codec);
            track->selected = false;
            all_wired = false;
        }
    }

    /* A rejected track means the chain would play with that track silently
     * missing — historically this shipped as a video-only cast with no error.
     * Tear down and escalate instead so the offending ES gets transcoded. */
    if (!all_wired)
    {
        var_DelCallback(hls_out, "hls-ready", OnHlsReady, p_sys);
        sout_StreamChainDelete(p_out, nullptr);
        cache->p_next        = nullptr;
        csys->hls_out        = nullptr;
        csys->hls_ready_fn   = nullptr;
        csys->hls_ready_data = nullptr;
        BuildFailCleanup(csys);

        if (p_sys->transcodingCanFallback())
        {
            p_sys->setNextTranscodingState();
            msg_Warn(p_stream, "unhandled stream — retrying with transcoding "
                               "video%s",
                     p_sys->transcoding_state & TRANSCODING_AUDIO ? "/audio"
                                                                 : "");
            csys->build_retry = true;
        }
        else
            p_sys->p_intf->requestPlayerStop();
        return false;
    }

    /* ---- Replay the pre-roll FIFO ---- */
    for (auto &ev : csys->events)
    {
        if (ev.is_pcr)
            sout_StreamSetPCR(p_out, ev.pcr);
        else if (ev.track->downstream_id)
            sout_StreamIdSend(p_out, ev.track->downstream_id, ev.block);
        else
            block_ChainRelease(ev.block);
    }
    csys->events.clear();

    csys->ready = true;
    msg_Dbg(p_stream, "HLS chain ready (transcoding=0x%x)", new_transcoding_state);
    p_sys->p_intf->setRetryOnFail(p_sys->transcodingCanFallback());
    return true;
}

/*****************************************************************************
 * Input event callback (from Chromecast control thread)
 *****************************************************************************/

static void on_input_event_cb(void *data, enum cc_input_event event,
                              union cc_input_arg arg)
{
    sout_stream_t     *p_stream = static_cast<sout_stream_t *>(data);
    sout_stream_sys_t *p_sys    =
        static_cast<sout_stream_sys_t *>(p_stream->p_sys);

    vlc_mutex_locker locker(&p_sys->lock);
    switch (event)
    {
        case CC_INPUT_EVENT_EOF:
            p_sys->cc_eof = arg.eof;
            if (p_sys->cc_eof)
            {
                cc_startup_cache_Reset(p_sys->p_cache);
                p_sys->cc_has_input = false;
            }
            /* When EOF lifts (cc_eof → false) the cache is already in
             * buffering mode from the earlier reset; the next PCR window
             * will rebuild the chain. */
            break;

        case CC_INPUT_EVENT_RETRY:
            cc_startup_cache_Reset(p_sys->p_cache);
            p_sys->cc_has_input = false;
            if (p_sys->transcodingCanFallback())
            {
                p_sys->setNextTranscodingState();
                msg_Warn(p_stream,
                         "Load failed detected. Switching to next "
                         "configuration. Transcoding video%s",
                         p_sys->transcoding_state & TRANSCODING_AUDIO
                             ? "/audio" : "");
            }
            break;
    }
}

/*****************************************************************************
 * Sout callbacks — thin forwarders to the startup cache
 *****************************************************************************/

static void *Add(sout_stream_t *p_stream, const es_format_t *p_fmt,
                 const char *es_id)
{
    sout_stream_sys_t *p_sys =
        static_cast<sout_stream_sys_t *>(p_stream->p_sys);
    vlc_mutex_locker locker(&p_sys->lock);

    if (!p_sys->b_supports_video && p_fmt->i_cat != AUDIO_ES)
        return nullptr;

    /* If the chain is live a new track forces a rebuild: tear down and let
     * the next PCR window call BuildHLSChain with the updated track set. */
    cc_startup_cache_sys_t *csys =
        static_cast<cc_startup_cache_sys_t *>(p_sys->p_cache->p_sys);
    if (csys->ready)
    {
        bool had_input = p_sys->cc_has_input;
        cc_startup_cache_Reset(p_sys->p_cache);
        p_sys->cc_has_input = false;
        if (had_input)
            p_sys->p_intf->requestPlayerStop();
    }

    return sout_StreamIdAdd(p_sys->p_cache, p_fmt, es_id);
}

static void Del(sout_stream_t *p_stream, void *id)
{
    sout_stream_sys_t *p_sys =
        static_cast<sout_stream_sys_t *>(p_stream->p_sys);
    vlc_mutex_locker locker(&p_sys->lock);

    cc_startup_cache_sys_t *csys =
        static_cast<cc_startup_cache_sys_t *>(p_sys->p_cache->p_sys);
    bool was_selected = static_cast<cc_cache_track_t *>(id)->selected;
    bool was_ready    = csys->ready;

    sout_StreamIdDel(p_sys->p_cache, id); /* → CacheDel; frees the track */

    if (was_ready && was_selected)
    {
        bool any_selected = false;
        for (auto *t : csys->tracks)
            if (t->selected) { any_selected = true; break; }

        if (!any_selected)
        {
            /* TODO: with HLS there is no single long-lived client to drain;
             * preserve unconditionally so the device can hand off without a
             * STOP.  Gate on an HLS-appropriate signal in a future pass. */
            p_sys->p_intf->preservePlaybackOnTeardown();
            cc_startup_cache_Reset(p_sys->p_cache);
            p_sys->cc_has_input = false;
            p_sys->p_intf->requestPlayerStop();
            p_sys->transcoding_state = TRANSCODING_NONE;
        }
    }
    else if (!was_ready && csys->tracks.empty())
        p_sys->transcoding_state = TRANSCODING_NONE;
}

static int Send(sout_stream_t *p_stream, void *id, block_t *p_buffer)
{
    sout_stream_sys_t *p_sys =
        static_cast<sout_stream_sys_t *>(p_stream->p_sys);
    vlc_mutex_locker locker(&p_sys->lock);

    if (p_sys->cc_eof)
    {
        block_ChainRelease(p_buffer);
        return VLC_SUCCESS;
    }

    return sout_StreamIdSend(p_sys->p_cache, id, p_buffer);
}

static void Flush(sout_stream_t *p_stream, void */* id */)
{
    sout_stream_sys_t *p_sys =
        static_cast<sout_stream_sys_t *>(p_stream->p_sys);
    vlc_mutex_locker locker(&p_sys->lock);

    /* Tear down the chain on the first flush; ignore subsequent per-track
     * flushes until the next PCR resets cc_flushing. */
    if (!p_sys->cc_flushing)
    {
        p_sys->cc_flushing  = true;
        bool had_input      = p_sys->cc_has_input;
        cc_startup_cache_Reset(p_sys->p_cache);
        p_sys->cc_has_input = false;
        if (had_input)
            p_sys->p_intf->requestPlayerStop();
    }
}

static void SetPCR(sout_stream_t *p_stream, vlc_tick_t pcr)
{
    sout_stream_sys_t *p_sys =
        static_cast<sout_stream_sys_t *>(p_stream->p_sys);
    vlc_mutex_locker locker(&p_sys->lock);

    if (p_sys->cc_eof)
        return;

    /* A PCR arriving after a flush means new data is flowing; the chain
     * rebuild will happen inside CacheSetPCR once the window elapses. */
    p_sys->cc_flushing = false;

    sout_StreamSetPCR(p_sys->p_cache, pcr);
}

static const struct sout_stream_operations ops = [] {
    struct sout_stream_operations o{};
    o.add     = Add;
    o.del     = Del;
    o.send    = Send;
    o.flush   = Flush;
    o.set_pcr = SetPCR;
    o.close   = Close;
    return o;
}();

/*****************************************************************************
 * Open: connect to the Chromecast and initialize the sout
 *****************************************************************************/
static int Open(vlc_object_t *p_this)
{
    sout_stream_t    *p_stream = reinterpret_cast<sout_stream_t *>(p_this);
    sout_stream_sys_t *p_sys   = nullptr;
    intf_sys_t       *p_intf   = nullptr;
    char             *psz_ip   = nullptr;
    httpd_host_t     *httpd_host = nullptr;
    bool              b_supports_video = true;
    int               i_local_server_port;
    int               i_device_port;

    config_ChainParse(p_stream, SOUT_CFG_PREFIX, ppsz_sout_options,
                      p_stream->p_cfg);

    psz_ip = var_GetNonEmptyString(p_stream, SOUT_CFG_PREFIX "ip");
    if (!psz_ip)
    {
        msg_Err(p_this, "missing Chromecast IP address");
        goto error;
    }

    i_device_port       = var_InheritInteger(p_stream, SOUT_CFG_PREFIX "port");
    i_local_server_port = var_InheritInteger(p_stream, SOUT_CFG_PREFIX "http-port");

    var_Create(p_stream, "http-port", VLC_VAR_INTEGER);
    var_SetInteger(p_stream, "http-port", i_local_server_port);
    var_Create(p_stream, "http-host", VLC_VAR_STRING);
    var_SetString(p_stream, "http-host", "");
    httpd_host = vlc_http_HostNew(VLC_OBJECT(p_stream));
    if (!httpd_host)
        goto error;

    try
    {
        p_intf = new intf_sys_t(p_this, i_local_server_port, psz_ip,
                                i_device_port, httpd_host);
    }
    catch (const std::runtime_error &err)
    {
        msg_Err(p_this, "cannot load the Chromecast controller (%s)",
                err.what());
        goto error;
    }
    catch (const std::bad_alloc &)
    {
        p_intf = nullptr;
        goto error;
    }

    b_supports_video = var_GetBool(p_stream, SOUT_CFG_PREFIX "video");

    try
    {
        p_sys = new sout_stream_sys_t(httpd_host, p_intf, b_supports_video,
                                      i_local_server_port);
    }
    catch (const std::exception &ex)
    {
        msg_Err(p_stream, "Failed to instantiate sout_stream_sys_t: %s",
                ex.what());
        p_sys = nullptr;
        goto error;
    }

    p_sys->p_cache = cc_startup_cache_New(p_stream, BuildHLSChain, p_stream);
    if (!p_sys->p_cache)
        goto error;

    p_intf->setOnInputEventCb(on_input_event_cb, p_stream);

    p_stream->ops   = &ops;
    p_stream->p_sys = p_sys;

    free(psz_ip);
    return VLC_SUCCESS;

error:
    delete p_intf;
    if (httpd_host)
        httpd_HostDelete(httpd_host);
    free(psz_ip);
    delete p_sys;
    return VLC_EGENERIC;
}

/*****************************************************************************
 * Close: destroy the interface and HTTP host
 *****************************************************************************/
static void Close(sout_stream_t *p_stream)
{
    sout_stream_sys_t *p_sys =
        static_cast<sout_stream_sys_t *>(p_stream->p_sys);

    /* By the sout contract every id added with Add has been Del'd; the track
     * list should therefore be empty. */
    assert(static_cast<cc_startup_cache_sys_t *>(p_sys->p_cache->p_sys)
               ->tracks.empty());

    /* CacheClose (invoked by sout_StreamChainDelete) handles final cleanup
     * of any live HLS chain and the cache sys. */
    sout_StreamChainDelete(p_sys->p_cache, nullptr);

    httpd_host_t *httpd_host = p_sys->httpd_host;
    delete p_sys->p_intf;
    delete p_sys;
    /* Delete last: p_intf and p_sys reference httpd_host. */
    httpd_HostDelete(httpd_host);
}
