struct hls_pacer
{
    bool is_enabled;
    vlc_tick_t deadline;
};

static inline vlc_tick_t hls_pacer_Pace(struct hls_pacer *pacer,
                                        vlc_tick_t pace_for)
{
    if (!pacer->is_enabled)
        return;

    const vlc_tick_t now = vlc_tick_now();

    vlc_tick_t last_deadline = pacer->deadline;
    if (last_deadline == VLC_TICK_INVALID)
        last_deadline = now;

    pacer->deadline = now + (pace_for - (now - last_deadline));
}
