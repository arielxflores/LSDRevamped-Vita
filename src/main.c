#include "common.h"
#include "video.h"

int plat_init_args(int argc, char **argv);

int main(int argc, char **argv)
{
    int w = 960, h = 544;
    if (!plat_init_args(argc, argv) || !plat_init(&w, &h))
        return 1;
    lsd_log("LSD Revamped Vita %s, starting (%dx%d)", LSD_VERSION, w, h);
    if (!game_init(w, h)) {
        lsd_log("init failed");
        plat_shutdown();
        return 1;
    }
    Input in;
    float dt;
    double last = plat_seconds(), next_frame = 0;
    while (!game_quit_requested() && plat_frame_begin(&in, &dt)) {
        double t0 = plat_seconds();
        game_update(&in, dt);
        double tu = plat_seconds();
        game_render();
        double t1 = plat_seconds();
        const char *shot = script_take_shot();
        if (shot) {
            shot_write(shot, w, h);
            plat_after_shot();
            video_note_freeze(); /* (a video's timing around it is not counted) */
        }
        plat_frame_end();
        double ts = plat_seconds();
        int limit = game_frame_limit(); /* LimitFramerate: Application.targetFrameRate = 20 */
        if (limit > 0) { /* (a steady beat: each frame's deadline follows the last one's) */
            double period = 1.0 / limit, now0 = plat_seconds();
            next_frame += period;
            if (next_frame < now0 - period || next_frame > now0 + period)
                next_frame = now0; /* (fell behind, or just switched on) */
            plat_wait_until(next_frame);
        } else {
            next_frame = 0;
        }
        double now = plat_seconds();
        if (!shot) {
            game_frame_stats(now - last, t1 - t0);
            /* (half a beat late or more: the display showed the last picture again) */
            double beat = limit > 0 ? 1.0 / limit : 1.0 / 60.0;
            if (now - last > beat * 1.5)
                game_hitch(now - last, tu - t0, t1 - tu, ts - t1);
        }
        prof_reset();
        last = now;
    }
    lsd_log("exiting");
    game_shutdown();
    plat_shutdown();
    return 0;
}
