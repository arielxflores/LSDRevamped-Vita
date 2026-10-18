/* Dream walking: LSDR's classic player controller (PlayerMovement.cs, PlayerRotation.cs,
 * PlayerCameraRotation.cs) on top of the baked dream; days and links are in dreamsys.c. */
#include "world.h"
#include "luaapi.h"
#include "scene.h"
#include "greyman.h"
#include "audio.h"
#include "title.h"
#include "skyfx.h"
#include "settings.h"
#include "controls.h"
#include "video.h"

/* LSDR Player.prefab values */
#define WALK_SPEED 0.6f
#define RUN_SPEED 2.4f
#define STEP_TIME 0.25f
#define HEAD_BOB 0.05f
#define GRAVITY (9.81f * 0.5f)
#define LINK_DELAY 0.7f
#define TURN_SPEED 70.0f
#define LOOK_SPEED 60.0f
#define LOOK_LIMIT 70.0f
#define CAMERA_HEIGHT (PLAYER_HEIGHT + 0.15f)
#define INTERACT_LOOK_BEHIND_TIME 0.5f

Player P;
Dream g_dream;
int g_dream_loaded;
static int g_w, g_h;

static struct {
    int long_draw;
    int overlay;
    int input_gate; /* test: no pad input on the title screen */
    int cmd_flag, cmd_this; /* a test command ran (before / during this frame) */
    int covered_before;     /* the picture on screen when this frame began was all fade */
    int pixelate, dither; /* -1 = follow the Classic/Revamped switch */
    float time;
    double clock; /* game time for shader animation */
    /* perf */
    double acc_frame, acc_cpu, max_frame, last_log;
    int frames;
    float fps, cpu_ms, max_ms;
    RenderStats rs;
    char msg[96];
    float msg_t;
    /* benchmark segment */
    char bench[48];
    int bench_frames;
    double bench_frame, bench_cpu, bench_worst;
    int bench_over;   /* frames slower than 1/60 s + 1 ms */
    long bench_tris;
} G;

void player_on_new_dream(void)
{
    render_rv_reset(); /* (the Revamped graphics back to the full size) */
    /* PlayerMovement.OnNewDream */
    P.colliding_t = 0;
    P.moving_into_wall = 0;
    P.locked_x = P.locked_y = 0;
    P.input_locked = 0;
    P.stepping = 0;
    P.bob = 0;
    P.grounded = 0;
}

void player_reset_view(void)
{
    P.look = P.behind = 0;
    P.sprinting = 0;
    P.bob = 0;
}

static void debug_spawn(int idx)
{
    if (!g_dream_loaded || g_dream.nspawn <= 0)
        return;
    idx = ((idx % g_dream.nspawn) + g_dream.nspawn) % g_dream.nspawn;
    const DreamSpawn *s = &g_dream.spawn[idx];
    P.feet = v3(s->pos.x + 0.01f, s->pos.y + PLAYER_SKIN, s->pos.z - 0.01f);
    P.yaw = s->yaw;
    player_reset_view();
    player_on_new_dream();
    P.can_link = 1;
    lsd_log("spawn %d at (%.2f %.2f %.2f) yaw %.0f", idx, P.feet.x, P.feet.y, P.feet.z, P.yaw);
}

int game_init(int width, int height)
{
    g_w = width;
    g_h = height;
    if (!render_init(width, height)) {
        plat_present_message("Shader setup failed.\nIs libshacccg.suprx installed?\nSee ux0:data/lsdr/log.txt");
        return 0;
    }
    if (!text_init())
        return 0;
    const char *pak = plat_pak_path();
    if (!vfs_init(pak, plat_data_dir())) {
        plat_present_message("The dream data (dreams.pak) is missing or incomplete.\n"
                             "Copy it to ux0:data/lsdr/dreams.pak\nSee ux0:data/lsdr/log.txt");
        return 0;
    }
    if (pak) {
        /* the rest of the data next to dreams.pak: dreams2.pak..., the videos (movies1.pak...), the
         * intro's openings (intro1.pak...) and the Revamped soundtrack (music1.pak...) */
        static const char *const KINDS[4] = {"dreams", "movies", "intro", "music"};
        char mp[256];
        for (int kind = 0; kind < 4; kind++) {
            int n = 0;
            for (int i = kind ? 1 : 2; i <= 9; i++) {
                snprintf(mp, sizeof(mp), "%s", pak);
                char *sl = strrchr(mp, '/');
                snprintf(sl ? sl + 1 : mp, sizeof(mp) - (size_t)(sl ? sl + 1 - mp : 0), "%s%d.pak", KINDS[kind], i);
                if (!vfs_add_pak_quiet(mp))
                    break;
                n++;
            }
            if (kind == 1 && !n)
                lsd_log("no videos (movies1.pak...): video days and in-dream videos end the day after their fade");
        }
    }
    /* LSD_PLAY=1 (PC harness): a script drives the pad, but the game starts like a normal launch */
    const char *play = getenv("LSD_PLAY");
    int test_run = script_active() && !(play && play[0] == '1');
    controls_init(!test_run);
    settings_init(!test_run);
    G.overlay = 0; /* (the test overlay: "cmd overlay 1") */
    G.pixelate = G.dither = -1;
    greyman_init();
    audio_init();
    P.can_link = 1;
    if (!ds_init(test_run)) {
        plat_present_message("Could not load the dream data.\nSee ux0:data/lsdr/log.txt");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ movement */

static vec3 forward_of(float yaw_deg)
{
    float a = yaw_deg * DEG2RAD;
    return v3(sinf(a), 0, cosf(a));
}

static vec3 right_of(float yaw_deg)
{
    float a = yaw_deg * DEG2RAD;
    return v3(cosf(a), 0, -sinf(a));
}

static void head_bob(float t)
{
    if (!S.head_bob) /* GameSettings.HeadBobEnabled */
        return;
    float y;
    if (S.smooth_head_bob) /* a sine wave (SmoothHeadBob) */
        y = (sinf(t * 2.0f * (float)M_PI - (float)M_PI / 2) + 1.0f) / 2.0f;
    else /* a triangle wave, as in the original */
        y = 2.0f * (0.5f - fabsf(fmodf(t, 1.0f) - 0.5f));
    P.bob = y * HEAD_BOB * S.head_bob_intensity;
}

static void move_controller(float in_x, float in_y, float speed, float dt)
{
    vec3 f = forward_of(P.yaw), r = right_of(P.yaw);
    vec3 move = v3add(v3scale(f, in_y), v3scale(r, in_x));
    move = v3scale(move, speed * dt);
    if (P.moving_into_wall) {
        move = v3(0, 0, 0);
        if (P.colliding_t > LINK_DELAY && P.can_link)
            ds_wall_link();
    } else {
        P.colliding_t = 0;
    }
    int flags = ds_col_move(&P.feet, move);
    P.grounded = (flags & COL_BELOW) != 0;
    if (flags & COL_SIDES)
        P.moving_into_wall = 1;
}

/* PlayerMovement.playFootstepSound: the LBD tile under the player picks the sound */
static void footstep(void)
{
    if (!S.footsteps || !g_dream.tri_chunk) /* GameSettings.EnableFootstepSounds */
        return;
    float y;
    int tri, sample;
    float pitch;
    vec3 p = ds_to_dream(P.feet);
    /* castController(capsule, down, skinWidth * 16) */
    if (!col_ray_down_tri(&g_dream, v3(p.x, p.y + 0.05f, p.z), 0.05f + PLAYER_SKIN * 16, &y, &tri))
        return;
    int ci = g_dream.tri_chunk[tri];
    int ok;
    if (ci >= g_dream.nfchunk) {
        ok = sfx_footstep_fallback(&sample, &pitch);
    } else {
        const FootChunk *fc = &g_dream.fchunk[ci];
        int x = (int)(P.feet.x - fc->pos[0] + 0.5f), z = (int)(P.feet.z - fc->pos[2] + 0.5f);
        if (x < 0 || x >= 20 || z < 0 || z >= 20) {
            ok = sfx_footstep_fallback(&sample, &pitch);
        } else {
            int t = x * 20 + z;
            if (t >= fc->ntile)
                return;
            ok = sfx_footstep(fc->footstep[t] & 0x7F, &sample, &pitch);
        }
    }
    if (ok)
        sfx_play(sample, pitch);
}

/* Start and Select act when released, and not while both are held (holding both quits) */
static uint32_t released_alone(const Input *in)
{
    static uint32_t armed;
    if (options_capturing()) { /* (a press the keyboard or a rebinding takes is not Start / Select) */
        armed = 0;
        return 0;
    }
    armed |= in->pressed & (BTN_SELECT | BTN_START);
    if ((in->held & (BTN_START | BTN_SELECT)) == (BTN_START | BTN_SELECT))
        armed &= ~(uint32_t)(BTN_START | BTN_SELECT);
    uint32_t released = armed & ~in->held;
    armed &= in->held;
    return released;
}

static void player_move(float in_x, float in_y, int run, float dt, int fps);
static void after_player(float dt);

/* PlayerCameraRotation.handleFpsCameraRotation + PlayerMovement with FpsControls */
static void fps_controls(float dt, const ControlScheme *scheme)
{
    float mx, my;
    ctl_move(&mx, &my);
    mx += ctl_strafe(); /* (the strafe buttons too) */
    float len = sqrtf(mx * mx + my * my);
    if (len > 1) {
        mx /= len;
        my /= len;
    }
    int run = ctl_held(ACT_RUN);
    float in_x = mx, in_y = my;
    if (P.input_locked) {
        in_x = P.locked_x;
        in_y = P.locked_y;
    } else if (!DS.can_control) {
        in_x = in_y = 0;
    } else if (g_player_ext.move_external) { /* (external input: tank controls, as LSDR forces) */
        in_x = g_player_ext.move_x > 0 ? 1.0f : g_player_ext.move_x < 0 ? -1.0f : 0.0f;
        in_y = g_player_ext.move_y > 0 ? 1.0f : g_player_ext.move_y < 0 ? -1.0f : 0.0f;
    }
    P.in_x = in_x;
    P.in_y = in_y;
    if (DS.can_control) {
        if (g_player_ext.rot_external) {
            P.yaw += g_player_ext.rot * TURN_SPEED * dt;
        } else if (!g_player_ext.move_external) {
            /* Look * DEGREES_PER_DOT * MouseSensitivity per frame in LSDR (so it turns faster at higher
             * frame rates); here per 1/120 s, a usable gamepad speed at the Vita's 60 */
            float lx, ly;
            ctl_look(&lx, &ly); /* (InvertLookY applied) */
            float k = 0.02199f * scheme->sensitivity * 120.0f * dt;
            if (S.limit_fps) k *= 4.0f / 3.0f;
            P.yaw += lx * k;
            P.look += ly * k;
            if (P.look > 70.0f) P.look = 70.0f; /* MaxY / MinY */
            if (P.look < -70.0f) P.look = -70.0f;
        }
        if (P.yaw > 360) P.yaw -= 360;
        if (P.yaw < 0) P.yaw += 360;
    }
    player_move(in_x, in_y, run, dt, 1);
    after_player(dt);
}

void game_update(const Input *in, float dt)
{
    static const Input NONE;
    if (G.input_gate && DS.mode == DS_TITLE) /* (test: random walks leave the title screen alone) */
        in = &NONE;
    G.cmd_this = G.cmd_flag;
    G.cmd_flag = 0;
    G.covered_before = game_screen_covered();
    G.time += dt;
    controls_update(in); /* (the current scheme's actions from the pad) */
    double tp = plat_seconds();
    audio_update(dt); /* (the music plays on while paused) */
    prof_add(PROF_AUDIO, plat_seconds() - tp);
    if (G.msg_t > 0) G.msg_t -= dt;
    uint32_t released = released_alone(in);
    int in_dream = DS.mode == DS_DREAM && g_dream_loaded && !DS.load_stage && !DS.playing_video;
    /* PauseButtonListener: Start toggles the pause */
    if ((released & BTN_START) && in_dream) {
        if (pause_active())
            pause_close();
        else if (DS.can_pause)
            pause_open();
    } else if (pause_active() && !in_dream) {
        pause_close();
    }
    if (pause_active()) { /* Time.timeScale = 0 */
        pause_update(in, dt);
        return;
    }
    G.clock += dt;
    tp = plat_seconds();
    ds_update(in, dt);
    prof_add(PROF_DS, plat_seconds() - tp);
    plat_cpu_boost(DS.load_stage > 0); /* (loading: the CPU at 444 MHz) */
    if (DS.mode != DS_DREAM || !g_dream_loaded || DS.load_stage)
        return;
    tp = plat_seconds();
    luaapi_tick(dt);
    prof_add(PROF_LUA, plat_seconds() - tp);
    /* SettingsProfileSwitcher: NextProfile released (Select: not while Start is held with it) */
    const Binding *np = &controls_current()->b[ACT_NEXT_PROFILE][0];
    int next_profile = np->ctl[0] == CTL_SELECT ? (released & BTN_SELECT) != 0 : ctl_released(ACT_NEXT_PROFILE);
    if (next_profile && in_dream) {
        settings_next_profile(); /* SettingsSystem.SwitchToNextProfile */
        G.pixelate = G.dither = -1;
    }
    P.colliding_t += dt;

    /* ---- input: the current control scheme's actions (controls.c; built in: "Classic", tank controls:
     * Move (left stick / d-pad) walks and turns, Strafe (L/R), Run (Cross), Interact (Circle; held:
     * look behind), LookUp / LookDown (Triangle / Square), and Look (the right stick) turns and looks
     * too; or "Revamped", FpsControls: Move walks and strafes any way, Look looks around freely) */
    const ControlScheme *scheme = controls_current();
    if (scheme->fps) {
        fps_controls(dt, scheme);
        return;
    }
    float mvx, mvy, lkx, lky;
    ctl_move(&mvx, &mvy);
    ctl_look(&lkx, &lky);
    float move_y = mvy, strafe = ctl_strafe(), turn = mvx + lkx, look = lky;
    if (ctl_held(ACT_LOOK_UP)) look += 1;
    if (ctl_held(ACT_LOOK_DOWN)) look -= 1;
    if (look > 1) look = 1;
    if (look < -1) look = -1;
    if (turn > 1) turn = 1;
    if (turn < -1) turn = -1;
    /* no partial movement in classic mode */
    float in_x = strafe > 0 ? 1.0f : strafe < 0 ? -1.0f : 0.0f;
    float in_y = move_y > 0 ? 1.0f : move_y < 0 ? -1.0f : 0.0f;
    int run = ctl_held(ACT_RUN);
    int interact = ctl_held(ACT_INTERACT);

    /* PlayerMovement.getInputDirection: locked input wins, otherwise nothing without control */
    if (P.input_locked) {
        in_x = P.locked_x;
        in_y = P.locked_y;
    } else if (!DS.can_control) {
        in_x = in_y = 0;
    } else if (g_player_ext.move_external) {
        /* a script is walking the player (PlayerMovement.UseExternalInput) */
        in_x = g_player_ext.move_x > 0 ? 1.0f : g_player_ext.move_x < 0 ? -1.0f : 0.0f;
        in_y = g_player_ext.move_y > 0 ? 1.0f : g_player_ext.move_y < 0 ? -1.0f : 0.0f;
    }
    P.in_x = in_x;
    P.in_y = in_y;

    if (DS.can_control) {
        /* ---- PlayerRotation */
        if (g_player_ext.rot_external)
            turn = g_player_ext.rot;
        P.yaw += turn * TURN_SPEED * dt;
        if (P.yaw > 360) P.yaw -= 360;
        if (P.yaw < 0) P.yaw += 360;

        /* ---- PlayerCameraRotation (tank mode) */
        if (look != 0) {
            P.look += look * LOOK_SPEED * dt;
            if (P.look > LOOK_LIMIT) P.look = LOOK_LIMIT;
            if (P.look < -LOOK_LIMIT) P.look = -LOOK_LIMIT;
        }
        if (ctl_pressed(ACT_INTERACT))
            P.interact_t = 0;
        P.interact_t += dt;
        if (interact) {
            if (P.interact_t > INTERACT_LOOK_BEHIND_TIME && P.behind < 180.0f)
                P.behind += LOOK_SPEED * 4 * dt;
            if (P.behind > 180.0f) P.behind = 180.0f;
        } else if (P.behind > 0) {
            P.behind -= LOOK_SPEED * 4 * dt;
            if (fabsf(P.behind) < 2) P.behind = 0;
        }
        if (look == 0 && !interact) {
            if (P.look < 0) P.look += LOOK_SPEED * dt;
            else if (P.look > 0) P.look -= LOOK_SPEED * dt;
            if (fabsf(P.look) < 2) P.look = 0;
        }
    }

    player_move(in_x, in_y, run, dt, 0);
    after_player(dt);
}

/* PlayerMovement.Update: classic steps (fixed quarter-second strides), or FPS walking */
static void player_move(float in_x, float in_y, int run, float dt, int fps)
{
    if (in_x == 0 && in_y == 0 && !P.stepping) {
        P.bob = 0;
        P.moving_into_wall = 0;
    }
    if (run && !g_player_ext.move_external && (in_x != 0 || in_y > 0))
        P.sprinting = 1;
    float speed = P.sprinting ? RUN_SPEED : WALK_SPEED;
    if (fps) {
        if (in_x != 0 || in_y != 0) {
            if (!P.stepping) {
                P.stepping = 1;
                P.step_t = 0;
            }
            if (!P.step_sounded && P.step_t > STEP_TIME / 2) {
                footstep();
                P.step_sounded = 1;
            }
            if (P.step_t > STEP_TIME) {
                if (P.sprinting) footstep();
                P.step_sounded = 0;
                P.step_t = 0;
            }
            head_bob(P.step_t / STEP_TIME);
            move_controller(in_x, in_y, speed, dt);
            P.step_t += dt; /* (_timeSinceStartedMoving) */
        } else {
            P.stepping = 0;
        }
    } else {
        if (!P.stepping && (in_x != 0 || in_y != 0)) {
            P.stepping = 1;
            P.step_sounded = 0;
            P.step_t = 0;
            P.step_in_x = in_x;
            P.step_in_y = in_y;
        }
        if (P.stepping) {
            P.step_t += dt;
            if (STEP_TIME - P.step_t < 0) {
                move_controller(P.step_in_x, P.step_in_y, speed, dt);
                if (P.sprinting) footstep(); /* the down step too when running */
                P.stepping = 0;
            } else {
                if (!P.step_sounded && P.step_t / STEP_TIME > 0.5f) { /* the top of the step */
                    P.step_sounded = 1;
                    footstep();
                }
                move_controller(P.step_in_x, P.step_in_y, speed, dt);
                head_bob(P.step_t / STEP_TIME);
            }
        }
    }
    if (DS.mode != DS_DREAM || !g_dream_loaded) /* a link may have started a load */
        return;
    if (!P.grounded && DS.gravity) {
        int flags = ds_col_move(&P.feet, v3(0, -GRAVITY * dt, 0));
        P.grounded = (flags & COL_BELOW) != 0;
    }
    if ((!run && in_x == 0 && in_y == 0) || g_player_ext.move_external)
        P.sprinting = 0;
}

static void after_player(float dt)
{
    if (DS.mode != DS_DREAM || !g_dream_loaded || DS.load_stage)
        return;
    ds_after_player(dt);
    if (DS.mode != DS_DREAM || !g_dream_loaded || DS.load_stage)
        return;
    scene_update(dt); /* the dream's entities: scripts, action chains, animation */
    greyman_update(dt);
    skyfx_update(dt);
}

/* ------------------------------------------------------------------ render */

static void camera(View *v)
{
    float yaw = (P.yaw + P.behind) * DEG2RAD;
    float pitch = P.look * DEG2RAD;
    vec3 f = v3(sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch));
    vec3 r = v3(cosf(yaw), 0, -sinf(yaw));
    vec3 u = v3cross(f, r); /* left-handed: up = forward x right */
    v->eye = v3(P.feet.x, P.feet.y + CAMERA_HEIGHT + P.bob, P.feet.z);
    v->fwd = f;
    v->right = r;
    v->up = u;
}

void game_render(void)
{
    View v;
    memset(&v, 0, sizeof(v));
    camera(&v);
    audio_listener(v.eye, v.fwd, v.right);
    v.fovy = S.fov;
    v.classic = S.classic;
    v.pixelate = G.pixelate >= 0 ? G.pixelate : S.pixelate;
    v.dither = G.dither >= 0 ? G.dither : S.dither;
    v.affine = S.affine;
    v.long_draw = G.long_draw || S.long_draw;
    v.flashback = DS.in_flashback && g_dream_loaded;
    v.texset = DS.texset;
    v.time = G.clock;
    v.env = ds_env();
    v.fade = DS.fade_a;
    memcpy(v.fade_rgb, DS.fade_rgb, sizeof(v.fade_rgb));
    double tp = plat_seconds();
    if (DS.mode == DS_TITLE && !g_dream_loaded) { /* titlescreen */
        title_render(&v, g_w, g_h);
        G.rs.drawn_batches = G.rs.drawn_tris = 0;
        prof_add(PROF_UI, plat_seconds() - tp);
        return;
    }
    if (DS.mode == DS_SPECIAL) { /* text_dream / video_dream */
        int pw = 0, ph = 0;
        GLuint pic = ds_special_picture(&pw, &ph);
        prof_add(PROF_TEX, plat_seconds() - tp);
        render_picture(pic, pw, ph, &v);
        G.rs.drawn_batches = G.rs.drawn_tris = 0;
        text_begin(g_w, g_h);
        text_end();
        return;
    }
    int show_world = g_dream_loaded && DS.mode == DS_DREAM && !DS.load_stage;
    if (show_world && !g_dream.tex[DS.texset & 3]) {
        if (!dream_load_texset(&g_dream, DS.texset))
            DS.texset = 0;
        prof_add(PROF_TEX, plat_seconds() - tp);
    }
    tp = plat_seconds();
    if (show_world) {
        const DreamEnv *e = v.env;
        float fe = (v.long_draw ? e->fog_end * 3.0f : e->fog_end) * g_rdbg.fog_mul;
        scene_build_draws(v.eye, fe);
        greyman_draws(v.eye, fe);
    }
    render_frame(show_world ? &g_dream : NULL, &v, &G.rs);
    prof_add(PROF_WORLD, plat_seconds() - tp);
    tp = plat_seconds();
    if (!show_world)
        G.rs.drawn_batches = G.rs.drawn_tris = 0;
    if (show_world)
        pause_render(g_w, g_h); /* (PauseCanvas, over the dream) */

    text_begin(g_w, g_h);
    if (G.overlay && show_world) {
        char line[256];
        int gx, gy;
        ds_graph_position(&gx, &gy);
        snprintf(line, sizeof(line), "LSD Revamped Vita %s | %s | Day %d | %s | %s", LSD_VERSION, g_dream.name, DS.day,
                 ds_texset_name(DS.texset), settings_profile_label());
        text_draw(8, 8, 2, 0xFFFFFFFFu, line);
        snprintf(line, sizeof(line), "%.1f fps  cpu %.1f ms  worst %.1f ms  batches %d  tris %d", G.fps, G.cpu_ms,
                 G.max_ms, G.rs.drawn_batches, G.rs.drawn_tris);
        text_draw(8, 28, 2, 0xFF80FFFFu, line);
        snprintf(line, sizeof(line), "pos %.1f %.1f %.1f  yaw %.0f  graph %d,%d  day ends in %.0f s", P.feet.x,
                 P.feet.y, P.feet.z, P.yaw, gx, gy, DS.timer > 0 ? DS.timer : 0.0f);
        text_draw(8, 48, 2, 0xFFFFFFFFu, line);
    }
    if (G.msg_t > 0)
        text_draw((float)g_w / 2 - (float)strlen(G.msg) * 8, (float)g_h / 2 - 60, 2, 0xFF60FFFFu, G.msg);
    text_end();
    prof_add(PROF_UI, plat_seconds() - tp);
}

/* ---- hitches: frames that missed the display's beat */
static struct {
    int test;               /* frames doing a test command's work */
    int seen, covered;      /* since the last report: could be seen / under a full fade */
    int gpu_hits;           /* frames the GPU held up, since gpu_t0 */
    double gpu_t0;
    int total_seen, total_covered;
    double worst_seen;
} H;

void game_frame_stats(double frame_s, double cpu_s)
{
    if (G.bench[0]) {
        G.bench_frames++;
        G.bench_frame += frame_s;
        G.bench_cpu += cpu_s;
        if (frame_s > G.bench_worst) G.bench_worst = frame_s;
        if (frame_s > 1.0 / 60.0 + 0.001) G.bench_over++;
        G.bench_tris += G.rs.drawn_tris;
    }
    G.acc_frame += frame_s;
    G.acc_cpu += cpu_s;
    if (frame_s > G.max_frame) G.max_frame = frame_s;
    G.frames++;
    if (G.acc_frame >= 1.0) {
        G.fps = (float)(G.frames / G.acc_frame);
        G.cpu_ms = (float)(G.acc_cpu / G.frames * 1000.0);
        G.max_ms = (float)(G.max_frame * 1000.0);
        if (G.time - G.last_log >= (script_active() ? 5.0 : 60.0)) { /* (a log line costs a write to the card) */
            G.last_log = G.time;
            plat_check_clocks();
            lsd_log("perf: %.1f fps, cpu %.2f ms avg, worst frame %.1f ms, %d batches %d tris, pos %.2f %.2f %.2f, "
                    "audio %.1f%%, hitches %d seen %d covered",
                    G.fps, G.cpu_ms, G.max_ms, G.rs.drawn_batches, G.rs.drawn_tris, P.feet.x, P.feet.y, P.feet.z,
                    plat_audio_load() * 100.0, H.total_seen, H.total_covered);
        }
        G.acc_frame = G.acc_cpu = G.max_frame = 0;
        G.frames = 0;
    }
}

/* nothing to see: all fade, or a video scene still black before its first picture */
int game_screen_covered(void)
{
    return DS.fade_a >= 0.99f || (DS.mode == DS_SPECIAL && DS.special_kind != 0 && !video_showing());
}

void game_hitch(double frame_s, double update_s, double render_s, double swap_s)
{
    /* (what stays on screen through a long frame is the picture before it) */
    int covered = G.covered_before;
    if (G.cmd_this || prof_test()) { /* a test command's own work (an instant dream load...): not something a player meets */
        H.test++;
    } else if (covered) {
        H.covered++;
        H.total_covered++;
    } else {
        H.seen++;
        H.total_seen++;
        if (frame_s > H.worst_seen) H.worst_seen = frame_s;
        /* the GPU behind the display with the full-size Revamped graphics (the frame waited on it, not
         * on the game): three times within three seconds, and they are drawn a step smaller */
        int pixelate = G.pixelate >= 0 ? G.pixelate : S.pixelate;
        if (!pixelate && DS.mode == DS_DREAM && !DS.load_stage && swap_s > update_s + render_s) {
            if (G.time - H.gpu_t0 > 3.0) {
                H.gpu_t0 = G.time;
                H.gpu_hits = 0;
            }
            if (++H.gpu_hits >= 3) {
                H.gpu_hits = 0;
                render_rv_lower();
            }
        }
    }
    static const char *const PART[PROF_N] = {"sound", "dream system", "scripts", "3D", "menus", "reading", "textures", "log"};
    char parts[256];
    size_t len = 0;
    parts[0] = 0;
    for (int i = 0; i < PROF_N; i++) {
        double t = prof_get(i);
        if (t >= 0.0015 && len < sizeof(parts) - 32)
            len += (size_t)snprintf(parts + len, sizeof(parts) - len, "%s %s %.1f", len ? "," : "", PART[i], t * 1000.0);
    }
    const char *mode = DS.mode == DS_DREAM ? (DS.load_stage ? "loading" : pause_active() ? "paused" : "dream")
                     : DS.mode == DS_SPECIAL ? (video_active() ? "video" : "text day")
                     : "title";
    lsd_log("hitch: %.1f ms (update %.1f, draw %.1f, swap %.1f;%s) %s, fade %.2f%s", frame_s * 1000.0,
            update_s * 1000.0, render_s * 1000.0, swap_s * 1000.0, parts[0] ? parts : " -", mode, DS.fade_a,
            G.cmd_this || prof_test() ? " (test work)" : covered ? " (covered)" : "");
}

static void hitch_report(const char *what)
{
    lsd_log("hitches %s: %d seen, %d under a full fade, %d test commands (all along: %d seen, worst %.1f ms; %d "
            "covered)",
            what, H.seen, H.covered, H.test, H.total_seen, H.worst_seen * 1000.0, H.total_covered);
    H.seen = H.covered = H.test = 0;
}

/* scripted commands (PC test harness and the Vita test route) */
int game_frame_limit(void) { return S.limit_fps ? 20 : 0; } /* SettingsSystem.FRAMERATE_LIMIT */

/* test commands that only look or set a flag (a long frame with one of these is the game's own) */
static int cheap_command(const char *cmd)
{
    static const char *const CHEAP[] = {"check", "where", "state", "graph", "luaerrors", "hitches", "uisel", "audio",
                                        "schemes", "settings", "clocks", "bench ", "link ", "linking ", "triggers ",
                                        "falling ", "timer ", "seed ", "inputgate ", "overlay ", "imepoll", "probe ",
                                        "specialdays "};
    for (size_t i = 0; i < sizeof(CHEAP) / sizeof(CHEAP[0]); i++)
        if (!strncmp(cmd, CHEAP[i], strlen(CHEAP[i])))
            return 1;
    return 0;
}

void game_command(const char *cmd)
{
    float a, b, c, d, e;
    int i;
    if (!cheap_command(cmd))
        G.cmd_flag = 1;
    if (settings_command(cmd) || controls_command(cmd)) {
    } else if (!strcmp(cmd, "uisel")) {
        options_log_selected();
    } else if (!strncmp(cmd, "imetext ", 8)) { /* test: what the system keyboard will give next */
        plat_text_input_set(cmd + 8);
    } else if (!strcmp(cmd, "imetest")) { /* test: the real keyboard opens (imepoll, imeabort) */
        lsd_log("keyboard test: %s", plat_text_input_begin("Keyboard test", "LSD", 31) ? "opened" : "could not open");
    } else if (!strcmp(cmd, "imepoll")) {
        char t[64];
        int r = plat_text_input_poll(t, sizeof(t));
        lsd_log("keyboard test: %s", r == 0 ? "still open" : r > 0 ? t : "closed");
    } else if (!strcmp(cmd, "imeabort")) {
        plat_text_input_abort();
    } else if (!strncmp(cmd, "titleview ", 10)) { /* test: a view of the title screen */
        title_show(cmd + 10);
    } else if (sscanf(cmd, "tp %f %f %f %f %f", &a, &b, &c, &d, &e) == 5) {
        P.feet = v3(a, b, c);
        P.yaw = d;
        P.look = e;
    } else if (sscanf(cmd, "spawn %d", &i) == 1) {
        debug_spawn(i);
    } else if (sscanf(cmd, "crc %d", &i) == 1) { /* test: CRC checks of unpacked data on/off */
        vfs_check_crc(i);
    } else if (sscanf(cmd, "keepawake %d", &i) == 1) { /* test: 0 = let the system idle as if nobody played */
        plat_test_keep_awake(i);
    } else if (!strcmp(cmd, "cpubench")) {
        plat_cpu_bench();
    } else if (!strcmp(cmd, "clocks")) {
        plat_check_clocks();
        plat_set_gpu_clock(0);
    } else if (sscanf(cmd, "rvscale %d", &i) == 1) { /* test: the Revamped graphics' size (% of the screen) */
        render_set_rv_scale(i);
    } else if (sscanf(cmd, "logthread %d", &i) == 1) { /* test: the log written by its thread (1) or at once (0) */
        plat_log_async(i);
    } else if (sscanf(cmd, "inputgate %d", &i) == 1) {
        G.input_gate = i;
    } else if (sscanf(cmd, "overlay %d", &i) == 1) {
        G.overlay = i;
    } else if (!strncmp(cmd, "bench begin ", 12)) {
        snprintf(G.bench, sizeof(G.bench), "%s", cmd + 12);
        G.bench_frames = G.bench_over = 0;
        G.bench_frame = G.bench_cpu = G.bench_worst = 0;
        G.bench_tris = 0;
    } else if (!strcmp(cmd, "hitches")) {
        hitch_report("since the last count");
    } else if (!strcmp(cmd, "bench end")) {
        if (G.bench[0] && G.bench_frames > 0) {
            int n = G.bench_frames;
            lsd_log("BENCH %-22s frames %4d  fps %6.1f  frame %6.2f ms  cpu %6.2f ms  worst %6.1f ms  slow %3d  tris %ld",
                    G.bench, n, n / G.bench_frame, G.bench_frame / n * 1000.0, G.bench_cpu / n * 1000.0,
                    G.bench_worst * 1000.0, G.bench_over, G.bench_tris / n);
        }
        G.bench[0] = 0;
    } else if (sscanf(cmd, "link %d", &i) == 1) {
        DS.no_wall_link = !i;
    } else if (sscanf(cmd, "vsync %d", &i) == 1) {
        plat_set_vsync(i);
        lsd_log("vsync %s", i ? "on" : "off");
    } else if (sscanf(cmd, "fogmul %f", &a) == 1) {
        g_rdbg.fog_mul = a;
    } else if (sscanf(cmd, "gpuclock %d", &i) == 1) {
        plat_set_gpu_clock(i);
    } else if (sscanf(cmd, "r nosky %d", &i) == 1) {
        g_rdbg.no_sky = i;
    } else if (sscanf(cmd, "r noworld %d", &i) == 1) {
        g_rdbg.no_world = i;
    } else if (sscanf(cmd, "r ditherhires %d", &i) == 1) {
        g_rdbg.dither_hires = i;
    } else if (sscanf(cmd, "r half %d", &i) == 1) {
        g_rdbg.half = i;
    } else if (sscanf(cmd, "r proj %d", &i) == 1) {
        g_rdbg.proj = i;
    } else if (sscanf(cmd, "r olddither %d", &i) == 1) {
        g_rdbg.old_dither = i;
    } else if (sscanf(cmd, "pixelate %d", &i) == 1) {
        G.pixelate = i;
    } else if (sscanf(cmd, "dither %d", &i) == 1) {
        G.dither = i;
    } else if (sscanf(cmd, "longdraw %d", &i) == 1) {
        G.long_draw = i;
    } else if (strcmp(cmd, "check") == 0) {
        /* collision sanity for the test harness */
        if (!g_dream_loaded) {
            lsd_log("check: no dream loaded");
            return;
        }
        float g;
        int in_wall = col_wall(&g_dream, ds_to_dream(P.feet));
        int has_floor = col_ground(&g_dream, ds_to_dream(P.feet), 0.01f, 50.0f, &g);
        if (has_floor) g = ds_from_dream(v3(0, g, 0)).y;
        int oob = P.feet.x < g_dream.lo[0] - 1 || P.feet.x > g_dream.hi[0] + 1 || P.feet.z < g_dream.lo[2] - 1 ||
                  P.feet.z > g_dream.hi[2] + 1;
        lsd_log("check: %s %.3f %.3f %.3f grounded=%d inwall=%d floor=%s%.3f oob=%d fade=%.2f", g_dream.dir, P.feet.x,
                P.feet.y, P.feet.z, P.grounded, in_wall, has_floor ? "" : "NONE ", has_floor ? g : 0.0f, oob,
                DS.fade_a);
    } else if (strcmp(cmd, "where") == 0) {
        lsd_log("where: %s %.3f %.3f %.3f yaw %.1f look %.1f grounded %d wall %d", g_dream_loaded ? g_dream.dir : "-",
                P.feet.x, P.feet.y, P.feet.z, P.yaw, P.look, P.grounded, P.moving_into_wall);
    } else if (!strncmp(cmd, "near ", 5)) {
        /* stand d units from an entity, facing it: near <id> <d> <yaw> */
        char id[128];
        float dist = 2, yaw = 0;
        if (sscanf(cmd + 5, "%127s %f %f", id, &dist, &yaw) >= 1) {
            int n = scene_find(id);
            if (n < 0) {
                lsd_log("near: no entity %s", id);
                return;
            }
            vec3 p = scene_world_pos(n);
            P.feet = v3(p.x - sinf(yaw * DEG2RAD) * dist, p.y + 0.01f, p.z - cosf(yaw * DEG2RAD) * dist);
            P.yaw = yaw;
            float g;
            if (col_ground(&g_dream, ds_to_dream(P.feet), 2.0f, 5.0f, &g)) P.feet.y = ds_from_dream(v3(0, g, 0)).y;
            lsd_log("near %s: entity at %.2f %.2f %.2f, player at %.2f %.2f %.2f", id, p.x, p.y, p.z, P.feet.x,
                    P.feet.y, P.feet.z);
        }
    } else if (!strncmp(cmd, "floormap ", 9)) { /* test: where there is something to stand on (a PGM, 4 px a unit) */
        char path[256];
        snprintf(path, sizeof(path), "%s", cmd + 9);
        FILE *f = g_dream_loaded ? fopen(path, "wb") : NULL;
        if (f) {
            int w = (int)((g_dream.hi[0] - g_dream.lo[0]) * 4) + 1, h = (int)((g_dream.hi[2] - g_dream.lo[2]) * 4) + 1;
            fprintf(f, "P5 %d %d 255\n", w, h);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    vec3 p = v3(g_dream.lo[0] + x * 0.25f, 1000.0f, g_dream.lo[2] + y * 0.25f);
                    float gy;
                    int any = col_anything_below(&g_dream, ds_to_dream(p), 0, PLAYER_RADIUS);
                    int fl = col_ray_down(&g_dream, ds_to_dream(p), &gy);
                    fputc(fl ? 255 : any ? 128 : 0, f);
                }
            fclose(f);
            lsd_log("floormap %s: %dx%d from %.1f %.1f", path, w, h, g_dream.lo[0], g_dream.lo[2]);
        }
    } else if (sscanf(cmd, "probe %f %f %f", &a, &b, &c) == 3) { /* test: the floors at a spot */
        if (g_dream_loaded) {
            vec3 p = v3(a, b, c);
            float top = 1000.0f, y;
            int n = 0;
            char buf[256];
            size_t len = 0;
            buf[0] = 0;
            while (n < 12 && col_ray_down(&g_dream, ds_to_dream(v3(a, top, c)), &y)) {
                len += (size_t)snprintf(buf + len, sizeof(buf) - len, " %.2f", y);
                top = y - 0.001f;
                n++;
                if (len >= sizeof(buf) - 8) break;
            }
            lsd_log("probe %.2f %.2f %.2f: anything below %d, floors at%s", a, b, c,
                    col_anything_below(&g_dream, ds_to_dream(p), PLAYER_RADIUS * 2, PLAYER_RADIUS), buf);
            int ti;
            if (col_ray_down_tri(&g_dream, ds_to_dream(v3(a, b + 1.0f, c)), 2.0f, &y, &ti)) {
                const ColTri *t = &g_dream.tri[ti];
                lsd_log("probe: triangle %d at %.3f, normal %.3f %.3f %.3f, (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f)",
                        ti, y, t->n.x, t->n.y, t->n.z, t->a.x, t->a.y, t->a.z, t->b.x, t->b.y, t->b.z, t->c.x, t->c.y, t->c.z);
            }
            float g;
            lsd_log("probe: col_ground %d", col_ground(&g_dream, ds_to_dream(v3(a, b + 0.02f, c)), 0.2f, 1.0f, &g));
        }
    } else if (!strcmp(cmd, "spawncheck")) { /* test: is there something under every spawn point */
        for (int k = 0; g_dream_loaded && k < g_dream.nspawn; k++) {
            const DreamSpawn *sp = &g_dream.spawn[k];
            vec3 f = v3(sp->pos.x + 0.01f, sp->pos.y + PLAYER_SKIN, sp->pos.z - 0.01f);
            float y = 0, y2 = 0;
            int below = col_anything_below(&g_dream, f, PLAYER_RADIUS * 2, PLAYER_RADIUS);
            int fl = col_ray_down(&g_dream, v3(f.x, f.y + 0.25f, f.z), &y);
            int fl2 = col_ray_down(&g_dream, f, &y2);
            float gy;
            int walk = col_ground(&g_dream, f, 0.25f, 0.5f, &gy);
            lsd_log("spawncheck %s %d (%.2f %.2f %.2f) flags %d: below %d, floor %s%.3f (under the feet %s%.3f), "
                    "walkable %d%s",
                    g_dream.dir, k, f.x, f.y, f.z, sp->flags, below, fl ? "" : "none ", fl ? y : 0.0f,
                    fl2 ? "" : "none ", fl2 ? y2 : 0.0f, walk, !below || (fl && y > f.y) || !walk ? "  <-- BAD" : "");
        }
    } else if (!strcmp(cmd, "entities")) {
        for (int i = 0; i < g_scene.nent; i++) {
            int n = g_scene.ent_node[i];
            vec3 p = scene_world_pos(n);
            lsd_log("entity %s at %.2f %.2f %.2f %s", g_scene.ent_id[i], p.x, p.y, p.z,
                    scene_active(n) ? "" : "(inactive)");
        }
    } else if (!ds_command(cmd)) {
        lsd_log("unknown command: %s", cmd);
    }
}

void game_query(float *x, float *y, float *z, int *grounded, int *wall)
{
    *x = P.feet.x;
    *y = P.feet.y;
    *z = P.feet.z;
    *grounded = P.grounded;
    *wall = P.moving_into_wall;
}

void game_shutdown(void)
{
    greyman_shutdown();
    luaapi_dream_end();
    scene_free();
    if (g_dream_loaded)
        dream_free(&g_dream);
    g_dream_loaded = 0;
    audio_shutdown();
    render_shutdown();
}

static int g_quit;
void game_request_quit(void) { g_quit = 1; }
int game_quit_requested(void) { return g_quit; }
