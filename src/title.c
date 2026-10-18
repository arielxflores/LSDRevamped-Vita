/* The title screen (titlescreen.unity): UIViewStack (MainMenu, Graph, Credits, Settings),
 * UICurrentDayDisplay, UICurrentJournalDisplay, UIYearsDisplay, UIBuildNumberDisplay, UIGraph,
 * UICredits, UIMainMenuMusic and UIButtonSounds, driven with the pad (Unity's button navigation).
 * The options (SettingsMenu.prefab: UITabView, the settings tabs, UIVideoSettingsProfile, the
 * reset-progress UIChoiceModal), here and in the pause menu (PauseCanvas.prefab: UIPauseMenu,
 * PauseSystem), drawn over the dream.
 *
 * On the Vita the options are drawn larger, the selected control's row lit, with button prompts under
 * them: L and R go through the tabs, a held direction (or the left stick) repeats, Triangle applies,
 * leaving them applies and saves too, and the touch screen works them (tap a control or its row, drag
 * a slider, drag a list). The menus take taps too. */
#include "title.h"
#include "ui.h"
#include "world.h"
#include "audio.h"
#include "render.h"
#include "settings.h"
#include "controls.h"

#define VIEW_FADE_SECS 1.0f /* UIViewStack.FadeDuration */
#define PRESS_FLASH_SECS 0.1f
#define CREDITS_SPEED 50.0f /* UICredits.ScrollSpeed (1 in titlescreen.unity) per FixedUpdate (50 a second), canvas units */
#define KOFI_URL "https://ko-fi.com/figglewatts"
#define SDK_URL "https://sdk.lsdrevamped.net"

static struct {
    int loaded;
    int stack[4], depth;   /* view indices */
    int sel;               /* selected button */
    int pending;           /* view to switch to when the fade-in finishes (-1 none); -2 = pop */
    int busy;              /* a transition or the day's start is under way */
    float press_t;
    int pressed;
    float time;
    int music_clip[4], click, select, sound_test;
    int playing;
    int view_main, view_graph, view_credits, view_settings, view_modal, view_pause, view_psettings;
    int view_mbg, view_bind, view_wait, view_pmbg, view_pbind, view_pwait; /* UIModalController's */
    float credits_y;       /* UICredits: SectionContainer.anchoredPosition.y, in pixels */
    int credits_group;
    int credits_over;
    char day[16], build[64];
} T;

/* ---------------------------------------------------------------- the options */

/* a direction held (the d-pad or the left stick) repeats */
typedef struct {
    float t[4];
    int on[4];
} NavRep;

typedef struct {
    int view, sel, pressed, tab;
    float press_t;
    int modal, modal_sel, modal_pressed;
    float modal_press_t;
    UiState st, mst;
    char text[UI_MAX_CTL][40];
    /* UIControlSchemeCreator: the creator shown in the controls tab (1 creating, 2 editing) */
    int creating, edit_index, g_selector, g_creator;
    ControlScheme made;    /* (Create: the new scheme until it is submitted) */
    int ime;               /* the name being typed on the system keyboard */
    /* UIModalController's stack: a binding choice, a wait for input over it */
    int v_bg, v_bind, v_wait;
    int bind_open, bind_action, bind_kind, bind_sel; /* bind_kind: 0 change, 1 reset, 2 clear */
    int wait_open, wait_action, wait_idx, wait_part;
    UiState bst, wst, bgst;
    char rows[UI_REBIND_ROWS][96], choice[UI_BIND_CHOICES][48], hint[96];
    char bind_title[48], wait_title[64], wait_text[48];
    /* the Vita's ways in: held directions (and the left stick), the touch screen, the prompts */
    NavRep nav;
    int was_touch, touch_mode, touch_ctl, touch_g, touch_prompt;
    float touch_x0, touch_y0, touch_ly;
    int dirty;             /* a setting changed since it was last applied */
    float toast_t;
    const char *toast;
    UiPrompt prompt[UI_MAX_PROMPTS];
    int prompt_act[UI_MAX_PROMPTS], nprompt;
    float prompt_rect[UI_MAX_PROMPTS][4];
} Options;

/* what a touched prompt does */
enum { PA_NONE, PA_TAB_PREV, PA_TAB_NEXT, PA_SELECT, PA_BACK, PA_APPLY };

static Options g_opt_title, g_opt_pause;

static int first_button_of(int view, int action) { return ui_button_by_action(view, action); }

static const char *const MOD_NAMES[] = {"Original Dreams"};
static const char *const JOURNAL_NAMES[] = {"Original Dreams"};

static int dropdown_count(int setting)
{
    return setting == SET_SCHEME ? controls_count() : 1; /* (UIControlSchemeDropdownPopulator) */
}

static const char *dropdown_label(int setting, int i)
{
    if (setting == SET_SCHEME)
        return controls_scheme(i)->name;
    return setting == SET_MOD ? MOD_NAMES[0] : JOURNAL_NAMES[0];
}

/* the scheme the creator works on: the new one, or (editing) the current one itself */
static Options *g_cur_opt;
static ControlScheme *edited(void)
{
    Options *o = g_cur_opt;
    if (!o || !o->creating)
        return NULL;
    return o->creating == 1 ? &o->made : controls_scheme_mut(o->edit_index);
}

static float get_setting(int id)
{
    switch (id) {
    case SET_CLASSIC: return (float)S.classic;
    case SET_PIXELATE: return (float)S.pixelate;
    case SET_DITHER: return (float)S.dither;
    case SET_LIMIT_FPS: return (float)S.limit_fps;
    case SET_LONG_DRAW: return (float)S.long_draw;
    case SET_FOV: return S.fov;
    case SET_AFFINE: return S.affine;
    case SET_HEAD_BOB: return (float)S.head_bob;
    case SET_SMOOTH_BOB: return (float)S.smooth_head_bob;
    case SET_BOB_INTENSITY: return S.head_bob_intensity;
    case SET_FOOTSTEPS: return (float)S.footsteps;
    case SET_SPECIAL_DAYS: return (float)S.special_days;
    case SET_MOD: return (float)S.mod;
    case SET_JOURNAL: return (float)S.journal;
    case SET_ORIGINAL_ST: return (float)S.original_soundtrack;
    case SET_MUSIC_VOL: return S.music_volume;
    case SET_SFX_VOL: return S.sfx_volume;
    case SET_SCHEME: return (float)controls_current_index();
    case SET_SCHEME_FPS: return edited() ? (float)edited()->fps : 0;
    case SET_SCHEME_INVERT: return edited() ? (float)edited()->invert_y : 0;
    case SET_SCHEME_SENS: return edited() ? edited()->sensitivity : 15;
    default: return 0;
    }
}

/* the two-way bindings (SettingsBindBroker) and the controls' onValueChanged */
static void set_setting(int id, float v)
{
    int i = (int)(v + (v < 0 ? -0.5f : 0.5f));
    switch (id) {
    case SET_CLASSIC: S.classic = i; break;
    case SET_PIXELATE: S.pixelate = i; break;
    case SET_DITHER: S.dither = i; break;
    case SET_LIMIT_FPS: S.limit_fps = i; break;
    case SET_LONG_DRAW: S.long_draw = i; break;
    case SET_FOV: S.fov = v; break;
    case SET_AFFINE: S.affine = v; break;
    case SET_HEAD_BOB: S.head_bob = i; break;
    case SET_SMOOTH_BOB: S.smooth_head_bob = i; break;
    case SET_BOB_INTENSITY: S.head_bob_intensity = v; break;
    case SET_FOOTSTEPS: S.footsteps = i; break;
    case SET_SPECIAL_DAYS: S.special_days = i; break;
    case SET_MOD: S.mod = i; break;
    case SET_JOURNAL: S.journal = i; break;
    case SET_ORIGINAL_ST: S.original_soundtrack = i; break;
    case SET_MUSIC_VOL: S.music_volume = v; audio_set_volumes(S.music_volume, S.sfx_volume); break;
    case SET_SFX_VOL: S.sfx_volume = v; audio_set_volumes(S.music_volume, S.sfx_volume); break;
    case SET_SCHEME: controls_select(i); break; /* UIControlSchemeCreator.SchemeDropdownFieldChanged */
    case SET_SCHEME_FPS: if (edited()) edited()->fps = i != 0; break;      /* UseFpsControlsOnValueChanged */
    case SET_SCHEME_INVERT: if (edited()) edited()->invert_y = i != 0; break; /* InvertLookYOnValueChanged */
    case SET_SCHEME_SENS: if (edited()) edited()->sensitivity = v; break;  /* MouseSensitivityOnValueChanged */
    default: break;
    }
    /* the video settings' controls: InvalidateCurrentProfile (the profile name gets its '*') */
    if (id >= SET_CLASSIC && id <= SET_AFFINE)
        settings_invalidate_profile();
    if (g_cur_opt && id < SET_SCHEME_FPS)
        g_cur_opt->dirty = 1;
}

/* the controls show the settings (and the profile panel, UIVideoSettingsProfile.UpdateView) */
static void opt_sync(Options *o)
{
    int n = ui_button_count();
    for (int b = 0; b < n && b < UI_MAX_CTL; b++) {
        const UiControlInfo *c = ui_control(b);
        if (c->view != o->view)
            continue;
        if (c->action >= UI_ACT_TAB && c->action < UI_ACT_TAB + 16) /* (the tab shown: its button marked) */
            o->st.on[b] = c->action - UI_ACT_TAB == o->tab;
        int disabled = !c->interactable;
        if (c->action == UI_ACT_PROFILE_DELETE)
            disabled = S.nprof <= 1;
        if (c->action == UI_ACT_SCHEME_EDIT || c->action == UI_ACT_SCHEME_DELETE)
            disabled = !controls_current()->editable; /* (UpdateView: the built-in schemes are not editable) */
        if (c->action == UI_ACT_SCHEME_CREATE)
            disabled = controls_count() >= MAX_SCHEMES;
        /* UIControlSchemeCreator.Show: the tabs, Apply and Back not interactable while it is up */
        if (o->creating && ((c->action >= UI_ACT_TAB && c->action < UI_ACT_TAB + 16) || c->action == UI_ACT_APPLY ||
                            c->action == UI_ACT_BACK))
            disabled = 1;
        if (c->setting == SET_SCHEME_NAME)
            disabled = o->creating != 1; /* (SchemeNameField.interactable only when creating) */
        if (c->action == UI_ACT_SCHEME_SUBMIT && edited()) /* (NameVerifier: a name, not one taken) */
            disabled = !controls_name_valid(edited()->name, o->creating == 2);
        if (disabled)
            o->st.state[b] = UI_BTN_DISABLED;
        else if (o->st.state[b] == UI_BTN_DISABLED)
            o->st.state[b] = UI_BTN_NORMAL;
        if (c->kind == UI_KIND_INPUT) {
            const char *nm = edited() ? edited()->name : "";
            o->st.ctl_text[b] = nm;
            o->st.on[b] = !nm[0]; /* (the placeholder while it is empty) */
            continue;
        }
        if (!c->setting)
            continue;
        float v = get_setting(c->setting);
        if (c->kind == UI_KIND_TOGGLE) {
            o->st.on[b] = v != 0;
        } else if (c->kind == UI_KIND_SLIDER) {
            float t = c->vmax > c->vmin ? (v - c->vmin) / (c->vmax - c->vmin) : 0;
            o->st.value[b] = t < 0 ? 0 : t > 1 ? 1 : t;
            /* UISliderHandleDisplay: Math.Round(value * ScaleFactor, DecimalPlaces) + PostFix */
            double sv = (double)v * c->tscale;
            snprintf(o->text[b], sizeof(o->text[b]), "%.*f%s", c->decimals, sv, c->postfix ? c->postfix : "");
            o->st.ctl_text[b] = o->text[b];
        } else if (c->kind == UI_KIND_DROPDOWN) {
            snprintf(o->text[b], sizeof(o->text[b]), "%s", dropdown_label(c->setting, (int)v));
            o->st.ctl_text[b] = o->text[b];
        }
    }
    /* UIVideoSettingsProfile: NextProfile's binding */
    char np[48];
    controls_binding_text(controls_current(), ACT_NEXT_PROFILE, 0, np, sizeof(np));
    snprintf(o->hint, sizeof(o->hint), "Use '%s' in-game to switch profiles", np);
    o->st.tag_text[UI_TAG_PROFILE_HINT] = o->hint;
    o->st.tag_text[UI_TAG_PROFILE_NAME] = settings_profile_label();
    /* the creator: its panel, its submit button's text, the rebind rows (UIRebindContainerPopulator) */
    if (o->g_selector >= 0) o->st.group_on[o->g_selector] = !o->creating;
    if (o->g_creator >= 0) o->st.group_on[o->g_creator] = o->creating != 0;
    o->st.tag_text[UI_TAG_SUBMIT_TEXT] = o->creating == 2 ? "Edit" : "Create";
    for (int a = 0; a < UI_REBIND_ROWS && a < NUM_REBINDABLE; a++) {
        controls_row_text(edited() ? edited() : controls_current(), a, o->rows[a], sizeof(o->rows[a]));
        o->st.tag_text[UI_TAG_ROW0 + a] = o->rows[a];
    }
}

void options_log_selected(void)
{
    Options *o = g_cur_opt;
    if (!o || o->sel < 0) {
        lsd_log("selected: nothing");
        return;
    }
    const UiControlInfo *c = ui_control(o->sel);
    lsd_log("selected: control %d (kind %d, action %d, setting %d) at %.0f,%.0f%s%s%s", o->sel, c->kind, c->action,
            c->setting, c->rect[0], c->rect[1], o->creating ? ", creator up" : "", o->bind_open ? ", choice up" : "",
            o->wait_open ? ", waiting for input" : "");
}

int options_capturing(void)
{
    Options *o = g_cur_opt;
    return o && (o->ime || o->wait_open || o->bind_open);
}

/* the scrolling list keeps the control selected in view */
static void opt_follow(Options *o)
{
    float sc;
    int g = ui_scroll_to_show(o->sel, &o->st, &sc);
    if (g >= 0)
        o->st.scroll[g] = sc;
}

static void opt_show_tab(Options *o, int tab)
{
    o->tab = tab;
    for (int g = 0; g < ui_group_count() && g < UI_MAX_GROUPS; g++)
        if (ui_group_view(g) == o->view && ui_group_tab(g) >= 0)
            o->st.group_on[g] = ui_group_tab(g) == tab;
}

static void opt_open(Options *o, int view)
{
    memset(o, 0, sizeof(*o));
    g_cur_opt = o;
    o->view = view;
    ui_state_init(&o->st, view);
    o->sel = -1;
    o->pressed = -1;
    o->g_selector = o->g_creator = -1;
    for (int g = 0; g < ui_group_count() && g < UI_MAX_GROUPS; g++)
        if (ui_group_view(g) == view) {
            if (ui_group_panel(g) == UI_PANEL_SCHEME_SELECTOR) o->g_selector = g;
            if (ui_group_panel(g) == UI_PANEL_SCHEME_CREATOR) o->g_creator = g;
        }
    int title = view == T.view_settings;
    o->v_bg = title ? T.view_mbg : T.view_pmbg;
    o->v_bind = title ? T.view_bind : T.view_pbind;
    o->v_wait = title ? T.view_wait : T.view_pwait;
    for (int b = 0; b < ui_button_count(); b++)
        if (ui_control(b)->view == view && ui_control(b)->selectme)
            o->sel = b; /* UISelectMe */
    opt_show_tab(o, 0); /* UITabView.OnEnable: StartOnTab */
    opt_sync(o);
}

static void opt_modal_open(Options *o)
{
    o->modal = 1;
    ui_state_init(&o->mst, T.view_modal);
    o->modal_sel = first_button_of(T.view_modal, UI_ACT_MODAL_CANCEL);
    o->modal_pressed = -1;
}

/* the first control of a kind (or a setting) shown in the view */
static int find_control(Options *o, int action, int setting)
{
    for (int b = 0; b < ui_button_count(); b++) {
        const UiControlInfo *c = ui_control(b);
        if (c->view == o->view && ((action && c->action == action) || (setting && c->setting == setting)))
            return b;
    }
    return -1;
}

/* UIControlSchemeCreator.Show / Hide */
static void creator_show(Options *o, int mode)
{
    o->creating = mode;
    o->edit_index = controls_current_index();
    if (mode == 1) /* new ControlScheme(ControlSchemeLoader.Current) */
        controls_new_from(&o->made, controls_current());
    for (int g = 0; g < UI_MAX_GROUPS; g++)
        if (ui_group_clip_height(g) > 0 && ui_group_view(g) == o->view)
            o->st.scroll[g] = 0;
    opt_sync(o);
    int b = find_control(o, 0, mode == 1 ? SET_SCHEME_NAME : SET_SCHEME_FPS);
    if (b >= 0) o->sel = b;
    lsd_log("scheme creator: %s", mode == 1 ? "create" : controls_current()->name);
}

static void creator_hide(Options *o)
{
    o->creating = 0;
    o->bind_open = o->wait_open = 0;
    opt_sync(o);
    int b = find_control(o, 0, SET_SCHEME);
    if (b >= 0) o->sel = b;
}

/* the binding choice modal (UIBindingChoiceModal: one button per binding) */
static void bind_open(Options *o, int action, int kind)
{
    static const char *const VERB[3] = {"change", "reset", "clear"};
    o->bind_open = 1;
    o->bind_action = action;
    o->bind_kind = kind;
    ui_state_init(&o->bst, o->v_bind);
    o->bind_sel = ui_button_by_action(o->v_bind, UI_ACT_BIND_CHOICE);
    snprintf(o->bind_title, sizeof(o->bind_title), "Choose binding to %s", VERB[kind]);
    o->bst.tag_text[UI_TAG_BIND_TITLE] = o->bind_title;
    o->bst.tag_text[UI_TAG_BIND_CANCEL] = "Press Select to cancel";
    for (int i = 0; i < UI_BIND_CHOICES; i++) {
        controls_binding_text(edited(), action, i < controls_bindings(action) ? i : 0, o->choice[i],
                              sizeof(o->choice[i]));
        o->bst.tag_text[UI_TAG_BIND_BUTTON0 + i] = o->choice[i];
    }
}

/* UIRebinder.InteractiveRebind: the waiting-for-input modal, then the pad */
static void wait_begin(Options *o, int action, int idx, int part)
{
    o->wait_open = 1;
    o->wait_action = action;
    o->wait_idx = idx;
    o->wait_part = part;
    ui_state_init(&o->wst, o->v_wait);
    if (controls_composite(action)) /* (ProvideTitleText: a composite part's name) */
        snprintf(o->wait_title, sizeof(o->wait_title), "Rebinding control for '%s'", part ? "positive" : "negative");
    else
        snprintf(o->wait_title, sizeof(o->wait_title), "Rebinding control");
    snprintf(o->wait_text, sizeof(o->wait_text), "Waiting for %s",
             controls_expects_vector(action) ? "Vector2" : controls_composite(action) ? "Axis" : "Button");
    o->wst.tag_text[UI_TAG_WAIT_TITLE] = o->wait_title;
    o->wst.tag_text[UI_TAG_WAIT_TEXT] = o->wait_text;
    o->wst.tag_text[UI_TAG_WAIT_CANCEL] = "Press Select to cancel";
    controls_capture_begin(controls_expects_vector(action));
}

static void binding_do(Options *o, int action, int idx, int kind)
{
    ControlScheme *e = edited();
    if (!e)
        return;
    if (kind == 0) {
        wait_begin(o, action, idx, 0);
        return;
    }
    if (kind == 1) /* IndexedActionBinding.ResetBinding */
        e->b[action][idx] = controls_default(action, idx);
    else /* DeleteBinding */
        e->b[action][idx].ctl[0] = e->b[action][idx].ctl[1] = CTL_NONE;
    lsd_log("binding: %s %d %s", controls_action_name(action), idx, kind == 1 ? "reset" : "cleared");
    o->bind_open = 0;
    opt_sync(o);
}

/* a row's rebind / reset / clear button: straight to it, or a choice when the action has more bindings */
static void row_press(Options *o, int action, int kind)
{
    if (!edited() || action < 0 || action >= NUM_REBINDABLE)
        return;
    if (controls_bindings(action) > 1)
        bind_open(o, action, kind);
    else
        binding_do(o, action, 0, kind);
}

/* the modals over the options; 1 if they took the input */
static int modals_update(Options *o, const Input *in)
{
    if (o->wait_open) {
        int r = controls_capture_poll(in);
        if (r == -2)
            return 1;
        if (r == -1) { /* cancelled (the choice modal goes too, on the same Select) */
            lsd_log("rebinding cancelled");
            o->wait_open = o->bind_open = 0;
            return 1;
        }
        ControlScheme *e = edited();
        if (e) {
            e->b[o->wait_action][o->wait_idx].ctl[o->wait_part] = (uint8_t)r;
            lsd_log("binding: %s %d%s -> %s", controls_action_name(o->wait_action), o->wait_idx,
                    controls_composite(o->wait_action) ? (o->wait_part ? " positive" : " negative") : "",
                    controls_ctl_name(r));
        }
        if (controls_composite(o->wait_action) && o->wait_part == 0) { /* (each part of a composite in turn) */
            wait_begin(o, o->wait_action, o->wait_idx, 1);
            return 1;
        }
        o->wait_open = o->bind_open = 0; /* (and the choice modal: HideModal on success) */
        sfx_play_clip(T.click, 1.0f);
        opt_sync(o);
        return 1;
    }
    if (o->bind_open) {
        static const uint32_t dirs[4] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
        for (int d = 0; d < 4; d++)
            if (in->pressed & dirs[d]) {
                int nb = ui_navigate(o->v_bind, &o->bst, o->bind_sel, d);
                if (nb >= 0 && ui_button_action(nb) - UI_ACT_BIND_CHOICE < controls_bindings(o->bind_action))
                    o->bind_sel = nb;
            }
        if (in->pressed & BTN_CROSS) {
            sfx_play_clip(T.click, 1.0f);
            binding_do(o, o->bind_action, ui_button_action(o->bind_sel) - UI_ACT_BIND_CHOICE, o->bind_kind);
        } else if (in->pressed & (BTN_SELECT | BTN_CIRCLE)) { /* (CancelAction) */
            o->bind_open = 0;
        }
        return 1;
    }
    return 0;
}

/* SettingsSystem.Apply, then Save (Apply, Triangle, and leaving the options) */
static void opt_apply(Options *o, int toast)
{
    settings_apply();
    settings_save();
    o->dirty = 0;
    if (toast) {
        o->toast = "Settings saved";
        o->toast_t = 1.5f;
    }
}

/* leaving the options: what was changed is kept (Unity's options needed Apply first) */
static void options_leave(Options *o)
{
    if (o->dirty) {
        lsd_log("options: applied and saved on leaving");
        opt_apply(o, 0);
    }
}

static int tab_count(Options *o)
{
    int n = 0;
    for (int b = 0; b < ui_button_count(); b++) {
        const UiControlInfo *c = ui_control(b);
        if (c->view == o->view && c->action >= UI_ACT_TAB && c->action < UI_ACT_TAB + 16 && c->action - UI_ACT_TAB + 1 > n)
            n = c->action - UI_ACT_TAB + 1;
    }
    return n;
}

/* the top control of a tab's panel (not the profile buttons beside it) */
static int first_in_tab(Options *o, int tab)
{
    int back = first_button_of(o->view, UI_ACT_BACK), pick = -1;
    float left = back >= 0 ? ui_control(back)->rect[0] - 1 : 0;
    for (int g = 0; g < ui_group_count(); g++) {
        if (ui_group_view(g) != o->view || ui_group_tab(g) != tab)
            continue;
        for (int b = 0; b < ui_button_count(); b++) {
            const UiControlInfo *c = ui_control(b);
            if (c->view != o->view || !ui_control_group_in(b, g) || o->st.state[b] == UI_BTN_DISABLED ||
                c->nav_mode == 0 || c->rect[0] < left)
                continue;
            int sub_shown = 1; /* (in a panel the game shows: only while it is) */
            for (int h = c->group; h >= 0 && h != g; h = -1)
                sub_shown = o->st.group_on[h];
            if (!sub_shown)
                continue;
            if (pick < 0 || c->rect[1] < ui_control(pick)->rect[1] - 0.5f ||
                (fabsf(c->rect[1] - ui_control(pick)->rect[1]) <= 0.5f && c->rect[0] < ui_control(pick)->rect[0]))
                pick = b;
        }
    }
    return pick;
}

/* L / R: the tab before or after, its top control selected (or its button, from the tab bar) */
static void opt_tab_step(Options *o, int dir)
{
    int n = tab_count(o);
    if (n <= 1 || o->creating)
        return;
    int on_bar = o->sel >= 0 && ui_control(o->sel)->group < 0;
    int t = (o->tab + dir + n) % n;
    opt_show_tab(o, t);
    sfx_play_clip(T.select, 1.0f);
    opt_sync(o);
    int b = on_bar ? first_button_of(o->view, UI_ACT_TAB + t) : first_in_tab(o, t);
    if (b < 0)
        b = first_button_of(o->view, UI_ACT_TAB + t);
    if (b >= 0) {
        o->sel = b;
        opt_follow(o);
    }
}

/* returns 1 to leave the options (Back), 2 for the credits */
static int opt_press(Options *o, int b)
{
    const UiControlInfo *c = ui_control(b);
    if (!c || o->st.state[b] == UI_BTN_DISABLED)
        return 0;
    if (c->kind == UI_KIND_BUTTON) {
        sfx_play_clip(T.click, 1.0f); /* UIButtonSounds */
        o->pressed = b;
        o->press_t = PRESS_FLASH_SECS;
    }
    int ret = 0;
    if (c->kind == UI_KIND_TOGGLE) {
        set_setting(c->setting, get_setting(c->setting) != 0 ? 0.0f : 1.0f);
    } else if (c->kind == UI_KIND_INPUT) { /* the system keyboard for the name */
        sfx_play_clip(T.click, 1.0f);
        ControlScheme *e = edited();
        if (e && plat_text_input_begin("Scheme name", e->name, SCHEME_NAME_MAX - 1))
            o->ime = 1;
    } else if (c->action >= UI_ACT_ROW_REBIND && c->action < UI_ACT_BIND_CHOICE) {
        int k = (c->action - UI_ACT_ROW_REBIND) / 8;
        row_press(o, (c->action - UI_ACT_ROW_REBIND) % 8, k);
    } else if (c->kind == UI_KIND_DROPDOWN) {
        int n = dropdown_count(c->setting);
        set_setting(c->setting, (float)(((int)get_setting(c->setting) + 1) % n));
    } else if (c->action >= UI_ACT_TAB && c->action < UI_ACT_TAB + 16) {
        opt_show_tab(o, c->action - UI_ACT_TAB);
    } else {
        switch (c->action) {
        case UI_ACT_BACK:
            options_leave(o);
            ret = 1;
            break;
        case UI_ACT_APPLY: /* SettingsSystem.Apply, then Save */
            opt_apply(o, 1);
            break;
        case UI_ACT_CREDITS: ret = 2; break;
        case UI_ACT_SOUND_TEST: sfx_play_clip(T.sound_test, 1.0f); break;
        case UI_ACT_PROFILE_NEXT: settings_next_profile(); o->dirty = 1; break;
        case UI_ACT_PROFILE_NEW: settings_new_profile(); o->dirty = 1; break;
        case UI_ACT_PROFILE_REVERT: settings_revert_profile(); o->dirty = 1; break;
        case UI_ACT_PROFILE_DELETE: settings_delete_profile(); o->dirty = 1; break;
        case UI_ACT_KOFI: plat_open_url(KOFI_URL); break;
        case UI_ACT_SDK: plat_open_url(SDK_URL); break;
        case UI_ACT_RESET_PROGRESS: opt_modal_open(o); break;
        case UI_ACT_SCHEME_CREATE: creator_show(o, 1); break; /* ShowCreate */
        case UI_ACT_SCHEME_EDIT: creator_show(o, 2); break;   /* ShowEdit */
        case UI_ACT_SCHEME_DELETE:                             /* DeleteScheme, SaveSchemes */
            controls_delete(controls_current_index());
            controls_save();
            o->sel = find_control(o, 0, SET_SCHEME);
            break;
        case UI_ACT_SCHEME_CANCEL: creator_hide(o); break; /* Hide */
        case UI_ACT_SCHEME_SUBMIT:                       /* SubmitScheme */
            if (o->creating == 1)
                controls_create(&o->made);
            controls_save();
            creator_hide(o);
            break;
        default: break;
        }
    }
    opt_sync(o);
    return ret;
}

/* Slider.OnMove along its axis: a twentieth of the range on the Vita (Unity moves a tenth; held, it
 * repeats), or 1 for whole numbers */
static int opt_adjust(Options *o, int b, int dir)
{
    const UiControlInfo *c = ui_control(b);
    if (!c || !c->setting || o->st.state[b] == UI_BTN_DISABLED)
        return 0;
    int step = dir == 2 ? -1 : 1;
    if (c->kind == UI_KIND_SLIDER) {
        float d = c->whole ? 1.0f : (c->vmax - c->vmin) * 0.05f;
        float v = get_setting(c->setting) + d * step;
        if (v < c->vmin) v = c->vmin;
        if (v > c->vmax) v = c->vmax;
        if (c->whole) v = floorf(v + 0.5f);
        set_setting(c->setting, v);
    } else {
        return 0; /* (a dropdown: Cross goes through its options; left and right go to the controls beside it) */
    }
    opt_sync(o);
    return 1;
}

/* a direction held (the d-pad or the left stick): once at once, then again and again after a moment */
#define REPEAT_DELAY 0.35f
static int nav_fires(NavRep *r, const Input *in, float dt, int d, float rate)
{
    static const uint32_t dirs[4] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
    float sx = in->lx, sy = in->ly;
    float th = r->on[d] ? 0.4f : 0.6f; /* (the stick: a little give before it lets go) */
    int stick = d == 0 ? sy < -th && fabsf(sy) >= fabsf(sx) : d == 1 ? sy > th && fabsf(sy) >= fabsf(sx)
              : d == 2 ? sx < -th && fabsf(sx) > fabsf(sy) : sx > th && fabsf(sx) > fabsf(sy);
    int on = (in->held & dirs[d]) != 0 || stick;
    if (!on) {
        r->on[d] = 0;
        return 0;
    }
    if (!r->on[d] || (in->pressed & dirs[d])) {
        r->on[d] = 1;
        r->t[d] = REPEAT_DELAY;
        return 1;
    }
    r->t[d] -= dt;
    int n = 0;
    while (r->t[d] <= 0 && n < 4) {
        r->t[d] += rate;
        n++;
    }
    return n;
}

static int dir_fires(Options *o, const Input *in, float dt, int d, float rate)
{
    return nav_fires(&o->nav, in, dt, d, rate);
}

/* the prompt a touch is on (-1 none) */
static int prompt_at(Options *o, float x, float y)
{
    for (int i = 0; i < o->nprompt; i++) {
        const float *r = o->prompt_rect[i];
        if (x >= r[0] - 8 && x < r[2] + 8 && y >= r[1] - 10 && y < r[3] + 10)
            return i;
    }
    return -1;
}

static void slider_to(Options *o, int b, float x)
{
    const UiControlInfo *c = ui_control(b);
    float t = ui_slider_value_at(b, x);
    if (!c || !c->setting || t < 0)
        return;
    float v = c->vmin + t * (c->vmax - c->vmin);
    if (c->whole) v = floorf(v + 0.5f);
    set_setting(c->setting, v);
    opt_sync(o);
}

static int opt_prompt_do(Options *o, int act)
{
    switch (act) {
    case PA_TAB_PREV: opt_tab_step(o, -1); break;
    case PA_TAB_NEXT: opt_tab_step(o, 1); break;
    case PA_APPLY:
        sfx_play_clip(T.click, 1.0f);
        opt_apply(o, 1);
        break;
    case PA_BACK:
        if (o->creating) {
            int b = find_control(o, UI_ACT_SCHEME_CANCEL, 0);
            if (b >= 0) return opt_press(o, b);
            return 0;
        } else {
            int b = first_button_of(o->view, UI_ACT_BACK);
            if (b >= 0) return opt_press(o, b);
            options_leave(o);
            return 1;
        }
    case PA_SELECT:
        if (o->sel >= 0) return opt_press(o, o->sel);
        break;
    default: break;
    }
    return 0;
}

/* the touch screen over the options: a tap on a control (or its row) is Cross on it; a finger on a
 * slider sets it where it is; dragging a list scrolls it; a tap on a prompt does what it says.
 * Returns what opt_press does (1 leave, 2 the credits) */
static int opt_touch(Options *o, const Input *in)
{
    int r = 0;
    if (in->touch && !o->was_touch) { /* (down) */
        o->touch_x0 = in->tx;
        o->touch_y0 = o->touch_ly = in->ty;
        o->touch_mode = 0;
        o->touch_g = ui_scroll_group_at(o->view, &o->st, in->tx, in->ty);
        o->touch_prompt = prompt_at(o, in->tx, in->ty);
        if (o->touch_prompt >= 0) {
            o->touch_mode = 4;
        } else {
            int b = ui_control_at(o->view, &o->st, in->tx, in->ty);
            o->touch_ctl = b;
            if (b >= 0) {
                o->sel = b;
                if (ui_control(b)->kind == UI_KIND_SLIDER) {
                    o->touch_mode = 2;
                    slider_to(o, b, in->tx);
                } else {
                    o->touch_mode = 1;
                }
            } else if (o->touch_g >= 0) {
                o->touch_mode = 5; /* (on a list, not on a control: a drag scrolls it) */
            }
        }
    } else if (in->touch && o->was_touch) { /* (moving) */
        if (o->touch_mode == 2) {
            slider_to(o, o->touch_ctl, in->tx);
        } else if ((o->touch_mode == 1 || o->touch_mode == 5) && o->touch_g >= 0 && fabsf(in->ty - o->touch_y0) > 12) {
            o->touch_mode = 3;
        }
        if (o->touch_mode == 3) {
            float max = ui_group_height(o->touch_g) - ui_group_clip_height(o->touch_g), sc;
            sc = o->st.scroll[o->touch_g] - (in->ty - o->touch_ly);
            o->st.scroll[o->touch_g] = sc < 0 ? 0 : sc > max ? (max > 0 ? max : 0) : sc;
        }
        o->touch_ly = in->ty;
    } else if (!in->touch && o->was_touch) { /* (up: a tap where it went down) */
        float x = in->tx, y = in->ty;
        if (o->touch_mode == 1 && ui_control_at(o->view, &o->st, x, y) == o->touch_ctl && o->touch_ctl >= 0) {
            o->sel = o->touch_ctl;
            r = opt_press(o, o->touch_ctl);
        } else if (o->touch_mode == 4 && prompt_at(o, x, y) == o->touch_prompt && o->touch_prompt >= 0) {
            r = opt_prompt_do(o, o->prompt_act[o->touch_prompt]);
        }
        o->touch_mode = 0;
    }
    o->was_touch = in->touch;
    return r;
}

static int opt_update(Options *o, const Input *in, float dt)
{
    if (o->press_t > 0 && (o->press_t -= dt) <= 0)
        o->pressed = -1;
    if (o->modal_press_t > 0 && (o->modal_press_t -= dt) <= 0)
        o->modal_pressed = -1;
    if (o->toast_t > 0)
        o->toast_t -= dt;
    if (o->ime) { /* (the keyboard is up: the pad is the dialog's) */
        char name[SCHEME_NAME_MAX];
        int r = plat_text_input_poll(name, sizeof(name));
        if (r != 0) {
            o->ime = 0;
            ControlScheme *e = edited();
            if (r > 0 && e) { /* SchemeNameFieldOnValueChanged (the font has the printable ASCII) */
                for (char *p = name; *p; p++)
                    if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '?';
                snprintf(e->name, sizeof(e->name), "%s", name);
                lsd_log("scheme name: %s", e->name);
            }
            opt_sync(o);
        }
        o->was_touch = in->touch;
        return 0;
    }
    if (o->bind_open && !o->wait_open && in->touch && !o->was_touch) { /* (a tap on a binding to choose) */
        int b = ui_control_at(o->v_bind, &o->bst, in->tx, in->ty);
        if (b >= 0 && ui_button_action(b) - UI_ACT_BIND_CHOICE < controls_bindings(o->bind_action)) {
            o->bind_sel = b;
            o->was_touch = 1;
            sfx_play_clip(T.click, 1.0f);
            binding_do(o, o->bind_action, ui_button_action(b) - UI_ACT_BIND_CHOICE, o->bind_kind);
            return 0;
        }
    }
    if (modals_update(o, in)) {
        o->was_touch = in->touch;
        return 0;
    }
    if (o->modal) { /* UIModalController: the choice modal over the options */
        for (int d = 0; d < 4; d++)
            if (dir_fires(o, in, dt, d, 0.15f)) {
                int nb = ui_navigate(T.view_modal, &o->mst, o->modal_sel, d);
                if (nb >= 0) o->modal_sel = nb;
            }
        int act = 0;
        if (in->pressed & BTN_CROSS)
            act = ui_button_action(o->modal_sel);
        else if (in->pressed & BTN_CIRCLE)
            act = UI_ACT_MODAL_CANCEL;
        if (in->touch && !o->was_touch) {
            int b = ui_control_at(T.view_modal, &o->mst, in->tx, in->ty);
            if (b >= 0) {
                o->modal_sel = b;
                act = ui_button_action(b);
            }
        }
        o->was_touch = in->touch;
        if (act == UI_ACT_MODAL_YES || act == UI_ACT_MODAL_CANCEL) {
            sfx_play_clip(T.click, 1.0f);
            if (act == UI_ACT_MODAL_YES) /* GameSave.CurrentJournalSave.ResetProgress, Save */
                ds_reset_progress();
            o->modal = 0;
        }
        return 0;
    }
    int r = opt_touch(o, in);
    if (r)
        return r;
    if (in->pressed & BTN_L)
        opt_tab_step(o, -1);
    else if (in->pressed & BTN_R)
        opt_tab_step(o, 1);
    for (int d = 0; d < 4; d++) {
        int slider = o->sel >= 0 && ui_control(o->sel)->kind == UI_KIND_SLIDER && d >= 2;
        int n = dir_fires(o, in, dt, d, slider ? 0.06f : 0.11f);
        for (int k = 0; k < n; k++) {
            if (d >= 2 && opt_adjust(o, o->sel, d))
                continue;
            int nb = ui_navigate(o->view, &o->st, o->sel, d);
            if (nb >= 0) {
                if (nb != o->sel) sfx_play_clip(T.select, 0.6f);
                o->sel = nb;
                opt_follow(o);
            }
        }
    }
    if (in->pressed & BTN_CROSS) {
        if (o->sel >= 0)
            return opt_press(o, o->sel);
    } else if (in->pressed & BTN_TRIANGLE) {
        return opt_prompt_do(o, PA_APPLY);
    } else if (in->pressed & BTN_CIRCLE) {
        return opt_prompt_do(o, PA_BACK);
    }
    return 0;
}

/* the prompts under the options, for what is up and what is selected */
static void opt_prompts(Options *o)
{
    int n = 0;
#define PROMPT(i0, l0, i1, l1, text, act)                                                                      \
    do {                                                                                                       \
        o->prompt[n].icon[0] = (i0);                                                                           \
        o->prompt[n].letter[0] = (l0);                                                                         \
        o->prompt[n].icon[1] = (i1);                                                                           \
        o->prompt[n].letter[1] = (l1);                                                                         \
        o->prompt[n].label = (text);                                                                           \
        o->prompt_act[n++] = (act);                                                                            \
    } while (0)
    if (o->wait_open) {
        PROMPT(-1, 0, -1, 0, "Press the new button or stick  (Select cancels)", PA_NONE);
    } else if (o->bind_open || o->modal) {
        PROMPT(o->bind_open ? UI_ICON_DPAD_UD : UI_ICON_DPAD_LR, 0, -1, 0, "Choose", PA_NONE);
        PROMPT(UI_ICON_CROSS, 0, -1, 0, "OK", PA_NONE);
        PROMPT(UI_ICON_CIRCLE, 0, -1, 0, "Cancel", PA_NONE);
    } else {
        const UiControlInfo *c = o->sel >= 0 ? ui_control(o->sel) : NULL;
        if (!o->creating)
            PROMPT(UI_ICON_SHOULDER, 'L', UI_ICON_SHOULDER, 'R', "Tabs", PA_TAB_NEXT);
        if (c && c->kind == UI_KIND_SLIDER)
            PROMPT(UI_ICON_DPAD_LR, 0, -1, 0, "Adjust", PA_NONE);
        else if (c)
            PROMPT(UI_ICON_CROSS, 0, -1, 0,
                   c->kind == UI_KIND_TOGGLE ? "Toggle" : c->kind == UI_KIND_DROPDOWN ? "Change"
                   : c->kind == UI_KIND_INPUT ? "Type" : "Select", PA_SELECT);
        PROMPT(UI_ICON_CIRCLE, 0, -1, 0, o->creating ? "Cancel" : "Back", PA_BACK);
        if (!o->creating)
            PROMPT(UI_ICON_TRIANGLE, 0, -1, 0, "Apply", PA_APPLY);
    }
#undef PROMPT
    o->nprompt = n;
}

static void opt_render(Options *o, int w, int h, float alpha)
{
    int n = ui_button_count();
    for (int b = 0; b < n && b < UI_MAX_CTL; b++) {
        if (ui_control(b)->view != o->view || o->st.state[b] == UI_BTN_DISABLED)
            continue;
        o->st.state[b] = b == o->pressed ? UI_BTN_PRESSED : b == o->sel ? UI_BTN_SELECTED : UI_BTN_NORMAL;
    }
    o->st.alpha = alpha;
    ui_draw(o->view, &o->st, w, h);
    if (o->bind_open || o->wait_open) { /* (one Background under the modal stack) */
        ui_state_init(&o->bgst, o->v_bg);
        o->bgst.alpha = alpha;
        ui_draw(o->v_bg, &o->bgst, w, h);
    }
    if (o->bind_open) {
        for (int b = 0; b < n && b < UI_MAX_CTL; b++)
            if (ui_control(b)->view == o->v_bind) {
                int i = ui_control(b)->action - UI_ACT_BIND_CHOICE;
                o->bst.state[b] = i >= controls_bindings(o->bind_action) ? UI_BTN_DISABLED
                                  : b == o->bind_sel                     ? UI_BTN_SELECTED
                                                                         : UI_BTN_NORMAL;
            }
        o->bst.alpha = alpha;
        ui_draw(o->v_bind, &o->bst, w, h);
    }
    if (o->wait_open) {
        o->wst.alpha = alpha;
        ui_draw(o->v_wait, &o->wst, w, h);
    }
    if (o->modal) {
        for (int b = 0; b < n && b < UI_MAX_CTL; b++)
            if (ui_control(b)->view == T.view_modal)
                o->mst.state[b] = b == o->modal_pressed ? UI_BTN_PRESSED
                                  : b == o->modal_sel   ? UI_BTN_SELECTED
                                                        : UI_BTN_NORMAL;
        ui_draw(T.view_modal, &o->mst, w, h);
    }
    /* the button prompts under Apply (or a word that the settings were saved) */
    int apply = first_button_of(o->view, UI_ACT_APPLY);
    float y = apply >= 0 ? ui_control(apply)->rect[3] + 7.0f : (float)h - 33.0f;
    if (y > (float)h - 31.0f) y = (float)h - 31.0f;
    opt_prompts(o);
    ui_prompt_rects(o->prompt, o->nprompt, w * 0.5f, y, o->prompt_rect);
    if (o->toast_t > 0 && o->toast)
        ui_draw_label(o->toast, w * 0.5f, y + 13.0f, alpha * (o->toast_t < 0.3f ? o->toast_t / 0.3f : 1.0f), w, h);
    else
        ui_draw_prompts(o->prompt, o->nprompt, w * 0.5f, y, alpha, w, h);
}

/* ---------------------------------------------------------------- the title screen */

static int top(void) { return T.stack[T.depth - 1]; }

/* a tap on one of a view's buttons: selected when the finger goes down, pressed when it lifts on it */
typedef struct {
    int was, ctl;
} Tap;

static int tap(Tap *t, const Input *in, int view, const UiState *st, int *sel)
{
    int r = -1;
    if (in->touch && !t->was) {
        t->ctl = ui_control_at(view, st, in->tx, in->ty);
        if (t->ctl >= 0)
            *sel = t->ctl;
    } else if (!in->touch && t->was) {
        if (t->ctl >= 0 && ui_control_at(view, st, in->tx, in->ty) == t->ctl)
            r = t->ctl;
        t->ctl = -1;
    }
    t->was = in->touch;
    return r;
}
static Tap g_title_tap, g_pause_tap;

/* UISelectMe: StartButton on the main menu, BackButton on the graph */
static void select_default(void)
{
    T.sel = top() == T.view_graph ? first_button_of(T.view_graph, UI_ACT_BACK) : first_button_of(T.view_main, UI_ACT_START);
    if (top() == T.view_settings)
        opt_open(&g_opt_title, T.view_settings);
}

/* the title screen's sounds (13 MB) are read off the card by a loading thread while the intro plays */
static const char *const PRELOAD[7] = {"ui/menu0.pcm", "ui/menu1.pcm", "ui/menu2.pcm", "ui/menu3.pcm",
                                       "ui/click.pcm", "ui/select.pcm", "ui/soundtest.pcm"};
static struct {
    uint8_t *buf[7];
    size_t n[7];
    int task, started;
} g_pre;

static void preload_job(void *arg)
{
    (void)arg;
    for (int i = 0; i < 7; i++)
        g_pre.buf[i] = vfs_read_bg(PRELOAD[i], &g_pre.n[i]);
}

void title_preload(void)
{
    if (g_pre.started || T.loaded)
        return;
    g_pre.started = 1;
    g_pre.task = plat_task_start(preload_job, NULL, 1);
}

static int clip(const char *path)
{
    for (int i = 0; i < 7; i++)
        if (g_pre.started && !strcmp(PRELOAD[i], path)) {
            int c = audio_clip_add(path, g_pre.buf[i], g_pre.n[i]);
            g_pre.buf[i] = NULL;
            return c;
        }
    return audio_clip_load(path);
}

static int load(void)
{
    if (T.loaded)
        return T.loaded > 0;
    if (g_pre.started)
        plat_task_join(g_pre.task); /* (long done, normally) */
    if (!ui_load()) {
        T.loaded = -1;
        return 0;
    }
    T.view_main = ui_view("MainMenu");
    T.view_graph = ui_view("Graph");
    T.view_credits = ui_view("Credits");
    T.view_settings = ui_view("Settings");
    T.view_modal = ui_view("Modal");
    T.view_pause = ui_view("Pause");
    T.view_psettings = ui_view("PauseSettings");
    T.view_mbg = ui_view("ModalBg");
    T.view_bind = ui_view("BindModal");
    T.view_wait = ui_view("WaitModal");
    T.view_pmbg = ui_view("PModalBg");
    T.view_pbind = ui_view("PBindModal");
    T.view_pwait = ui_view("PWaitModal");
    T.credits_group = -1;
    for (int g = 0; g < ui_group_count(); g++)
        if (ui_group_view(g) == T.view_credits && ui_group_tab(g) < 0)
            T.credits_group = g;
    for (int i = 0; i < 4; i++)
        T.music_clip[i] = clip(PRELOAD[i]);
    T.click = clip("ui/click.pcm");
    T.select = clip("ui/select.pcm");
    T.sound_test = clip("ui/soundtest.pcm");
    T.loaded = T.view_main >= 0 ? 1 : -1;
    return T.loaded > 0;
}

/* UIMainMenuMusic: Normal, Kanji, Downer or Upper by the day number */
static void start_music(void)
{
    int m = DS.day % 41;
    int k = m <= 10 ? 0 : m <= 20 ? 1 : m <= 30 ? 2 : 3;
    audio_bgm_play(T.music_clip[k]);
    T.playing = 1;
}

void title_stop_music(void)
{
    if (T.playing) {
        audio_bgm_stop();
        T.playing = 0;
    }
}

void title_enter(int from_dream)
{
    if (!load())
        return;
    T.depth = 1;
    T.stack[0] = T.view_main;
    /* UITitleScreenInitialisation: back from a dream, the graph is shown first */
    if (from_dream && T.view_graph >= 0)
        T.stack[T.depth++] = T.view_graph;
    T.pending = -1;
    T.busy = 0;
    T.pressed = -1;
    select_default();
    start_music();
}

int title_active(void) { return T.loaded > 0 && T.depth > 0; }

static void push(int view)
{
    if (T.busy || T.depth >= 4 || view < 0)
        return;
    T.busy = 1;
    T.pending = view;
    ds_fade_title(1.0f, VIEW_FADE_SECS);
}

static void pop(void)
{
    if (T.busy || T.depth <= 1)
        return;
    T.busy = 1;
    T.pending = -2;
    ds_fade_title(1.0f, VIEW_FADE_SECS);
}

/* UIViewStack.transitionBetween: switch views under the black, then fade back out */
void title_fade_done(void)
{
    if (T.pending == -2) {
        if (T.depth > 1) T.depth--;
    } else if (T.pending >= 0) {
        T.stack[T.depth++] = T.pending;
    }
    T.pending = -1;
    if (top() == T.view_credits) { /* UICredits.OnEnable */
        T.credits_y = 0;
        T.credits_over = 0;
    }
    select_default();
    ds_fade_title(0.0f, VIEW_FADE_SECS);
    T.busy = 0;
}

static void press(int b)
{
    int act = ui_button_action(b);
    if (act == UI_ACT_FLASHBACK && !ds_can_flashback())
        return; /* (not interactable: UIFlashbackButton) */
    sfx_play_clip(T.click, 1.0f);
    T.pressed = b;
    T.press_t = PRESS_FLASH_SECS;
    switch (act) {
    case UI_ACT_START:
        T.busy = 1;
        lsd_log("title: start");
        ds_start_day();
        break;
    case UI_ACT_FLASHBACK: /* DreamSystem.BeginFlashback */
        T.busy = 1;
        lsd_log("title: flashback");
        ds_begin_flashback();
        break;
    case UI_ACT_GRAPH:
        push(T.view_graph);
        break;
    case UI_ACT_BACK:
        pop();
        break;
    case UI_ACT_EXIT:
        lsd_log("title: exit");
        game_request_quit();
        break;
    case UI_ACT_OPTIONS:
        push(T.view_settings);
        break;
    default:
        break; /* (the journal arrows: there is only one journal) */
    }
}

void title_update(const Input *in, float dt)
{
    if (!title_active())
        return;
    T.time += dt;
    if (T.press_t > 0 && (T.press_t -= dt) <= 0)
        T.pressed = -1;
    if (top() == T.view_credits && !T.busy) {
        /* UICredits: scrolls up, ends past its height (+500), or on Run (Cross) */
        T.credits_y += CREDITS_SPEED * 1.0431f * dt;
        float end = ui_group_height(T.credits_group) + 500.0f * 1.0431f;
        int tapped = in->touch && !g_title_tap.was; /* (a tap ends them too) */
        g_title_tap.was = in->touch;
        if (!T.credits_over && (ctl_held(ACT_RUN) || T.credits_y > end || tapped)) {
            T.credits_over = 1;
            pop(); /* OnCreditsEnd */
        }
        return;
    }
    if (T.busy || DS.in_sequence)
        return;
    if (top() == T.view_settings) {
        int r = opt_update(&g_opt_title, in, dt);
        if (r == 1)
            pop(); /* OnReturnToTitle: UIViewStack.Pop */
        else if (r == 2)
            push(T.view_credits); /* OnCreditsShouldOpen */
        return;
    }
    static NavRep nav; /* (the d-pad or the left stick, held: again and again) */
    for (int d = 0; d < 4; d++) {
        for (int k = nav_fires(&nav, in, dt, d, 0.15f); k > 0; k--) {
            /* (explicit navigation, as Unity does it, lands on a button that is not interactable too) */
            int nb = ui_button_nav(T.sel, d);
            if (nb >= 0)
                T.sel = nb;
        }
    }
    static UiState tst; /* (what is there to tap) */
    ui_state_init(&tst, top());
    int fb = first_button_of(T.view_main, UI_ACT_FLASHBACK);
    if (fb >= 0 && !ds_can_flashback())
        tst.state[fb] = UI_BTN_DISABLED;
    int tb = tap(&g_title_tap, in, top(), &tst, &T.sel);
    if (tb >= 0) {
        press(tb);
    } else if (in->pressed & BTN_CROSS) {
        if (T.sel >= 0)
            press(T.sel);
    } else if ((in->pressed & BTN_CIRCLE) && top() == T.view_graph) {
        int b = first_button_of(T.view_graph, UI_ACT_BACK);
        if (b >= 0) press(b);
    }
}

void title_render(View *v, int w, int h)
{
    /* the title camera's background */
    render_clear(0.09411765f, 0.03137255f, 0.25882354f);
    if (title_active()) {
        if (top() == T.view_settings) {
            opt_render(&g_opt_title, w, h, 1.0f);
            render_fade(v);
            return;
        }
        static UiState st;
        ui_state_init(&st, top());
        st.time = T.time;
        int n = ui_button_count();
        for (int b = 0; b < n && b < UI_MAX_CTL; b++) {
            int act = ui_button_action(b);
            st.state[b] = (act == UI_ACT_FLASHBACK && !ds_can_flashback()) ? UI_BTN_DISABLED
                          : b == T.pressed                                 ? UI_BTN_PRESSED
                          : b == T.sel                                     ? UI_BTN_SELECTED
                                                                           : UI_BTN_NORMAL;
        }
        if (T.credits_group >= 0)
            st.scroll[T.credits_group] = T.credits_y;
        /* UICurrentDayDisplay */
        snprintf(T.day, sizeof(T.day), "Day %03d", DS.day);
        st.tag_text[UI_TAG_DAY] = T.day;
        static const float DAY_COLS[4][4] = {
            {0.78f, 0.553f, 0.635f, 1}, {0.533f, 0.498f, 0.773f, 1}, {0.431f, 0.8f, 0.518f, 1}, {1, 1, 1, 1}};
        int m = DS.day % 41;
        st.tag_color_set[UI_TAG_DAY] = 1;
        memcpy(st.tag_color[UI_TAG_DAY], DAY_COLS[m <= 10 ? 0 : m <= 20 ? 1 : m <= 30 ? 2 : 3], sizeof(float) * 4);
        /* UICurrentJournalDisplay, UIBuildNumberDisplay, UIYearsDisplay */
        st.tag_text[UI_TAG_JOURNAL] = "Original Dreams";
        snprintf(T.build, sizeof(T.build), "0.3.0-pre.5 (Vita %s)", LSD_VERSION);
        st.tag_text[UI_TAG_BUILD] = T.build;
        st.years = DS.year;
        /* UIGraph */
        st.nsquares = DS.nhist < UI_MAX_SQUARES ? DS.nhist : UI_MAX_SQUARES;
        int first = DS.nhist - st.nsquares;
        for (int i = 0; i < st.nsquares; i++) {
            st.sq_x[i] = DS.hist_x[first + i];
            st.sq_y[i] = DS.hist_y[first + i];
        }
        ui_draw(top(), &st, w, h);
    }
    render_fade(v);
}

/* test: open a view of the title screen directly */
void title_show(const char *name)
{
    if (!load())
        return;
    int v = ui_view(name);
    if (v < 0)
        return;
    T.depth = 1;
    T.stack[0] = T.view_main;
    if (v != T.view_main)
        T.stack[T.depth++] = v;
    T.busy = 0;
    T.pending = -1;
    if (v == T.view_credits) {
        T.credits_y = 0;
        T.credits_over = 0;
    }
    select_default();
}

/* ---------------------------------------------------------------- the pause menu */

static struct {
    int open;
    int sel, pressed;
    float press_t;
    int in_settings; /* the SettingsMenu pushed on the pause view stack */
    int stick_wait;  /* (the stick was held when it opened, walking: not a menu move until let go) */
} PZ;

int pause_active(void) { return PZ.open; }

/* PauseSystem.Pause: time stops (the music plays on); UISelectMe picks RESUME */
void pause_open(void)
{
    if (PZ.open || !load() || T.view_pause < 0)
        return;
    PZ.open = 1;
    PZ.in_settings = 0;
    PZ.sel = first_button_of(T.view_pause, UI_ACT_RESUME);
    PZ.pressed = -1;
    PZ.press_t = 0;
    PZ.stick_wait = 1;
    lsd_log("paused");
}

void pause_close(void)
{
    if (!PZ.open)
        return;
    if (PZ.in_settings) /* (Start out of the options keeps what was changed, as Back does) */
        options_leave(&g_opt_pause);
    PZ.open = 0;
    PZ.in_settings = 0;
    lsd_log("unpaused");
}

static void pause_press(int b)
{
    int act = ui_button_action(b);
    sfx_play_clip(T.click, 1.0f);
    PZ.pressed = b;
    PZ.press_t = PRESS_FLASH_SECS;
    switch (act) {
    case UI_ACT_RESUME:
        pause_close();
        break;
    case UI_ACT_MAIN_MENU: /* UIPauseMenu.ReturnToMenu */
        pause_close();
        ds_force_end_dream();
        break;
    case UI_ACT_EXIT: /* UIPauseMenu.QuitGame */
        lsd_log("pause: quit");
        game_request_quit();
        break;
    case UI_ACT_SKIP_SONG: /* DreamSystem.SkipSong */
        music_skip();
        break;
    case UI_ACT_OPTIONS: /* OnOptionsShouldOpen: PushWithoutTransition */
        if (T.view_psettings >= 0) {
            PZ.in_settings = 1;
            opt_open(&g_opt_pause, T.view_psettings);
        }
        break;
    default:
        break;
    }
}

void pause_update(const Input *in, float dt)
{
    if (!PZ.open)
        return;
    Input still;
    if (PZ.stick_wait) {
        if (fabsf(in->lx) < 0.3f && fabsf(in->ly) < 0.3f) {
            PZ.stick_wait = 0;
        } else {
            still = *in;
            still.lx = still.ly = 0;
            in = &still;
        }
    }
    if (PZ.press_t > 0 && (PZ.press_t -= dt) <= 0)
        PZ.pressed = -1;
    if (PZ.in_settings) {
        if (opt_update(&g_opt_pause, in, dt) == 1) /* OnReturnToTitle: PopWithoutTransition */
            PZ.in_settings = 0;
        return;
    }
    static NavRep nav;
    for (int d = 0; d < 4; d++) {
        for (int k = nav_fires(&nav, in, dt, d, 0.15f); k > 0; k--) {
            int nb = ui_button_nav(PZ.sel, d);
            if (nb >= 0)
                PZ.sel = nb;
        }
    }
    static UiState tst;
    ui_state_init(&tst, T.view_pause);
    int tb = tap(&g_pause_tap, in, T.view_pause, &tst, &PZ.sel);
    if (tb >= 0) {
        pause_press(tb);
    } else if (in->pressed & BTN_CROSS) {
        if (PZ.sel >= 0)
            pause_press(PZ.sel);
    } else if (in->pressed & BTN_CIRCLE) {
        pause_close(); /* (the Vita's back button: the same as RESUME) */
    }
}

void pause_render(int w, int h)
{
    if (!PZ.open || T.view_pause < 0)
        return;
    if (PZ.in_settings) {
        opt_render(&g_opt_pause, w, h, 1.0f);
        return;
    }
    static UiState st;
    ui_state_init(&st, T.view_pause);
    int n = ui_button_count();
    for (int b = 0; b < n && b < UI_MAX_CTL; b++)
        st.state[b] = b == PZ.pressed ? UI_BTN_PRESSED : b == PZ.sel ? UI_BTN_SELECTED : UI_BTN_NORMAL;
    /* UIPauseMenu.UpdateSongText */
    const char *song = music_current_song();
    st.tag_text[UI_TAG_SONG] = song ? song : "No song playing";
    st.tag_text[UI_TAG_ARTIST] = song ? music_current_author() : "";
    ui_draw(T.view_pause, &st, w, h);
}
