/// This file is included in libVLC main page in the doxygen documentation.

//! [minimal example]
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <vlc/vlc.h>
#include "vlc/libvlc_media_player.h"

int main(int argc, char* argv[])
{
    (void) argc; (void) argv;
    libvlc_instance_t * inst;
    libvlc_media_player_t *mp;
    libvlc_media_t *m;

    const char * av[] = {
        "-vv",
        NULL
    };
    /* Load the VLC engine */
    inst = libvlc_new (1, av);

    /* Create a new item */
    m = libvlc_media_new_path("../../../vlc.js/archive/navalgroup.mkv");
    //m = libvlc_media_new_path("/path/to/test.mov");

    /* Create a media player playing environement */
    mp = libvlc_media_player_new_from_media (inst, m);

    /* No need to keep the media now */
    libvlc_media_release (m);

    /* play the media_player */
    libvlc_media_player_play (mp);

    sleep (1);
    libvlc_media_player_select_tracks_by_ids(mp, libvlc_track_video, "video/1,video/2");
    sleep (1);

    libvlc_media_track_t *v_track = libvlc_media_player_get_track_from_id(mp, "video/2");
    assert(v_track != NULL);
    libvlc_media_track_t *s_track = libvlc_media_player_get_track_from_id(mp, "spu/10");
    assert(s_track != NULL);

    libvlc_media_player_MapSPUToVideo(mp, s_track, v_track);
    libvlc_media_track_release(v_track);

    sleep (2);
    libvlc_media_player_UnmapSPUToVideo(mp, s_track);

    libvlc_media_track_release(s_track);
    while (libvlc_media_player_is_playing(mp))
    {
        sleep (1);
        int64_t milliseconds = libvlc_media_player_get_time(mp);
        int64_t seconds = milliseconds / 1000;
        int64_t minutes = seconds / 60;
        milliseconds -= seconds * 1000;
        seconds -= minutes * 60;

        printf("Current time: %" PRId64 ":%" PRId64 ":%" PRId64 "\n",
               minutes, seconds, milliseconds);
    }

    /* Stop playing */
    libvlc_media_player_stop_async (mp);

    /* Free the media_player */
    libvlc_media_player_release (mp);

    libvlc_release (inst);

    return 0;
}
//! [minimal example]
