/*****************************************************************************
 * bridge.c: test for stream output bridging
 *****************************************************************************
 * Copyright (C) 2023 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
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

/* Define a builtin module for mocked parts */
#define MODULE_NAME test_bridge_mock
#undef VLC_DYNAMIC_PLUGIN

#include <assert.h>

#include <vlc_common.h>

#include <vlc_frame.h>
#include <vlc_input_item.h>
#include <vlc_interface.h>
#include <vlc_player.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>

#include "../../libvlc/test.h"

const char vlc_module_name[] = MODULE_STRING;

struct bridge_scenario
{
    const char *source;
    const char *sout;
};

struct es_data
{
    unsigned frame_received_count;
};

struct test_scenario
{
#define MAX_BRIDGES (0x10u)
    const struct bridge_scenario *bridges[MAX_BRIDGES];

    //    void (*report_error)(int);
    void (*report_frame)(const vlc_frame_t *, struct es_data*);
    void (*report_pcr)(vlc_tick_t);
    void (*report_new_es)(const char *);
};

static struct scenario_data
{
    vlc_sem_t wait_stop;
    vlc_tick_t last_pcr;

#define MAX_ES_DATA (0x10u)
    struct es_data es_data[MAX_ES_DATA];
    unsigned es_data_count; 

} scenario_data;

static void scenario_data_init(void)
{
    vlc_sem_init(&scenario_data.wait_stop, 0);
    scenario_data.last_pcr = VLC_TICK_INVALID;
    memset(scenario_data.es_data, 0, sizeof(scenario_data.es_data));
    scenario_data.es_data_count = 0;
}

static void CountFramePerES(const vlc_frame_t *frame, struct es_data *es_data)
{
    (void)frame;
    es_data->frame_received_count++;
}

static void ValidateFramesAtSecondPCR(vlc_tick_t pcr)
{
    if (scenario_data.last_pcr == VLC_TICK_INVALID)
    {
        scenario_data.last_pcr = pcr;
        return;
    }

    for (unsigned i = 0; i < scenario_data.es_data_count; ++i)
        assert(scenario_data.es_data[i].frame_received_count > 0);

    vlc_sem_post(&scenario_data.wait_stop);
}

struct bridge_scenario v = {
    .source = "mock://"
              "video_track_count=1;length=100000000000;"
              "video_width=800;video_height=600",
    .sout = "sout=#bridge-in:output_checker",
};

struct bridge_scenario a_out = {
    .source = "mock://audio_track_count=1;length=100000000000",
    .sout = "sout=#bridge-out",
};

struct test_scenario test_scenarios[] = {
    {
        .bridges = {&v, &a_out},
        .report_frame = CountFramePerES,
        .report_pcr = ValidateFramesAtSecondPCR,
    },
};

size_t current_scenario = 0u;

static int OutputCheckerSend(sout_stream_t *stream, void *id, vlc_frame_t *f)
{
    (void)stream;
    (void)id;
    struct test_scenario *scenario = &test_scenarios[current_scenario];

    assert(scenario->report_frame != NULL);
    scenario->report_frame(f, id);

    vlc_frame_Release(f);

    return VLC_SUCCESS;
}

static void *OutputCheckerAdd(sout_stream_t *stream,
                              const es_format_t *fmt,
                              const char *es_id)
{
    (void)stream; (void)fmt;

    struct test_scenario *scenario = &test_scenarios[current_scenario];
    if (scenario->report_new_es != NULL)
        scenario->report_new_es(es_id);

    assert(scenario_data.es_data_count < MAX_ES_DATA);
    return &scenario_data.es_data[scenario_data.es_data_count++];
}

static void OutputCheckerDel(sout_stream_t *stream, void *id)
{
    (void)stream; (void)id;
}

static void OutputCheckerSetPCR(sout_stream_t *stream, vlc_tick_t pcr)
{
    (void)stream;
    struct test_scenario *scenario = &test_scenarios[current_scenario];

    assert(scenario->report_pcr != NULL);
    scenario->report_pcr(pcr);
}

static int OpenOutputChecker(vlc_object_t *obj)
{
    sout_stream_t *stream = (sout_stream_t *)obj;

    static const struct sout_stream_operations ops = {
        .add = OutputCheckerAdd,
        .del = OutputCheckerDel,
        .send = OutputCheckerSend,
        .set_pcr = OutputCheckerSetPCR,
    };
    stream->ops = &ops;
    return VLC_SUCCESS;
}

static void
on_state_changed(vlc_player_t *player, enum vlc_player_state state, void *latch)
{
    (void)player;
    if (state == VLC_PLAYER_STATE_STOPPED)
        vlc_latch_count_down(latch, 1);
}
static void play_scenario(intf_thread_t *intf, struct test_scenario *scenario)
{
    scenario_data_init();

    struct played_scenario
    {
        vlc_player_t *player;
        vlc_player_listener_id *listener;
        input_item_t *media;
        vlc_cond_t cond;
    };

    struct VLC_VECTOR(struct played_scenario) played_scenarios =
        VLC_VECTOR_INITIALIZER;
    size_t i = 0;
    for (; i < MAX_BRIDGES; ++i)
    {
        const struct bridge_scenario *bridge = scenario->bridges[i];
        if (bridge == NULL)
            break;
        struct played_scenario played_scenario;
        vlc_cond_init(&played_scenario.cond);
        played_scenario.media = input_item_New(bridge->source, "dummy");
        assert(played_scenario.media);
        input_item_AddOption(
            played_scenario.media, bridge->sout, VLC_INPUT_OPTION_TRUSTED);

        played_scenario.player =
            vlc_player_New(&intf->obj, VLC_PLAYER_LOCK_NORMAL, NULL, NULL);
        assert(played_scenario.player);
        static const struct vlc_player_cbs player_cbs = {
            .on_state_changed = on_state_changed,
        };
        vlc_player_Lock(played_scenario.player);
        played_scenario.listener = vlc_player_AddListener(
            played_scenario.player, &player_cbs, &played_scenario.cond);
        vlc_player_SetCurrentMedia(played_scenario.player,
                                   played_scenario.media);
        vlc_player_Start(played_scenario.player);
        vlc_player_Unlock(played_scenario.player);
    }

    vlc_sem_wait(&scenario_data.wait_stop);

    struct played_scenario played_scenario;
    vlc_vector_foreach (played_scenario, &played_scenarios)
    {
        vlc_player_Lock(played_scenario.player);
        vlc_player_Stop(played_scenario.player);

        while (vlc_player_GetState(played_scenario.player) !=
               VLC_PLAYER_STATE_STOPPED)
            vlc_player_CondWait(played_scenario.player, &played_scenario.cond);

        vlc_player_RemoveListener(played_scenario.player,
                                  played_scenario.listener);
        vlc_player_Unlock(played_scenario.player);

        vlc_player_Delete(played_scenario.player);
        input_item_Release(played_scenario.media);
    }
}

static int OpenIntf(vlc_object_t *obj)
{
    intf_thread_t *intf = (intf_thread_t *)obj;

    while (current_scenario < ARRAY_SIZE(test_scenarios))
    {
        msg_Info(intf, " - Running scenario %zu", current_scenario);
        play_scenario(intf, &test_scenarios[current_scenario]);
        current_scenario++;
    }

    return VLC_SUCCESS;
}
/**
 * Inject the mocked modules as a static plugin:
 *  - access for triggering the correct decoder
 *  - decoder for generating video format and context
 *  - filter for generating video format and context
 *  - encoder to check the previous video format and context
 **/
// clang-format off
vlc_module_begin()
    set_capability("interface", 0)
    set_callback(OpenIntf)

    add_submodule()
        set_callback(OpenOutputChecker)
        set_capability("sout output", 0)
        add_shortcut("output_checker")
vlc_module_end();
// clang-format on

VLC_EXPORT const vlc_plugin_cb vlc_static_modules[] = {VLC_SYMBOL(vlc_entry),
                                                       NULL};

int main()
{
#ifndef ENABLE_SOUT
    (void)libvlc_playlist_play;
    return 77;
#endif
    test_init();

    const char *const args[] = {"-vvv",
                                "--vout=dummy",
                                "--aout=dummy",
                                "--text-renderer=dummy",
                                "--no-auto-preparse"};

    libvlc_instance_t *vlc = libvlc_new(ARRAY_SIZE(args), args);

    libvlc_add_intf(vlc, MODULE_STRING);
    libvlc_playlist_play(vlc);

    libvlc_release(vlc);
    assert(ARRAY_SIZE(test_scenarios) == current_scenario);
    return 0;
}
