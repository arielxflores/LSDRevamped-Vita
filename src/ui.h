/* LSD Revamped's Unity UI screens, laid out by tools/uibake.py (ui.bin) and drawn here */
#ifndef LSD_UI_H
#define LSD_UI_H

#include "common.h"

enum { UI_TAG_DAY = 1, UI_TAG_JOURNAL = 2, UI_TAG_BUILD = 3, UI_TAG_YEARS = 4, UI_TAG_GRAPH = 5, UI_TAG_SONG = 6,
       UI_TAG_ARTIST = 7, UI_TAG_PROFILE_HINT = 8, UI_TAG_PROFILE_NAME = 9, UI_TAG_SUBMIT_TEXT = 10,
       UI_TAG_ROW0 = 20, /* + row: a rebind row's action text */
       UI_TAG_BIND_TITLE = 28, UI_TAG_BIND_CANCEL = 29, UI_TAG_BIND_BUTTON0 = 30, /* + choice */
       UI_TAG_WAIT_TITLE = 33, UI_TAG_WAIT_TEXT = 34, UI_TAG_WAIT_CANCEL = 35 };
#define UI_NTAGS 40
/* (tools/uibake.py ACTIONS and CLICKS) */
enum { UI_ACT_START = 1, UI_ACT_FLASHBACK, UI_ACT_GRAPH, UI_ACT_OPTIONS, UI_ACT_EXIT, UI_ACT_LEFT, UI_ACT_RIGHT,
       UI_ACT_BACK, UI_ACT_RESUME, UI_ACT_MAIN_MENU, UI_ACT_SKIP_SONG, UI_ACT_APPLY, UI_ACT_CREDITS,
       UI_ACT_SOUND_TEST, UI_ACT_PROFILE_NEXT, UI_ACT_PROFILE_NEW, UI_ACT_PROFILE_REVERT, UI_ACT_PROFILE_DELETE,
       UI_ACT_SCHEME_EDIT, UI_ACT_SCHEME_DELETE, UI_ACT_SCHEME_CREATE, UI_ACT_RESET_PROGRESS, UI_ACT_KOFI,
       UI_ACT_SDK, UI_ACT_MODAL_YES, UI_ACT_MODAL_CANCEL, UI_ACT_SCHEME_CANCEL = 30, UI_ACT_SCHEME_SUBMIT = 31,
       UI_ACT_TAB = 32, /* + tab */
       UI_ACT_ROW_REBIND = 64, UI_ACT_ROW_RESET = 72, UI_ACT_ROW_CLEAR = 80, /* + row */
       UI_ACT_BIND_CHOICE = 88 /* + choice */ };
#define UI_REBIND_ROWS 7
#define UI_BIND_CHOICES 2
enum { UI_BTN_NORMAL, UI_BTN_SELECTED, UI_BTN_PRESSED, UI_BTN_DISABLED };
enum { UI_KIND_BUTTON, UI_KIND_TOGGLE, UI_KIND_SLIDER, UI_KIND_DROPDOWN, UI_KIND_INPUT };
/* panels the game shows or hides (UIControlSchemeCreator: the selector or the creator) */
enum { UI_PANEL_SCHEME_SELECTOR = 1, UI_PANEL_SCHEME_CREATOR = 2 };
/* the settings the options' controls are bound to (tools/uibake.py SETTING_FIELDS) */
enum { SET_CLASSIC = 1, SET_PIXELATE, SET_DITHER, SET_LIMIT_FPS, SET_LONG_DRAW, SET_FOV, SET_AFFINE, SET_HEAD_BOB,
       SET_SMOOTH_BOB, SET_BOB_INTENSITY, SET_FOOTSTEPS, SET_SPECIAL_DAYS, SET_MOD, SET_JOURNAL, SET_ORIGINAL_ST,
       SET_MUSIC_VOL, SET_SFX_VOL, SET_SCHEME, SET_SCHEME_FPS, SET_SCHEME_INVERT, SET_SCHEME_SENS, SET_SCHEME_NAME };

#define UI_MAX_CTL 256
#define UI_MAX_GROUPS 32
#define UI_MAX_SQUARES 512

typedef struct {
    uint8_t state[UI_MAX_CTL];        /* UI_BTN_* */
    uint8_t on[UI_MAX_CTL];           /* toggles (an input field: its placeholder shown) */
    float value[UI_MAX_CTL];          /* sliders: 0..1 along the bar */
    const char *ctl_text[UI_MAX_CTL]; /* a slider's value text, a dropdown's caption, an input field's text (NULL = as baked) */
    uint8_t group_on[UI_MAX_GROUPS];  /* tab panels and game-shown panels shown */
    float scroll[UI_MAX_GROUPS];      /* scrolling content: pixels moved up */
    const char *tag_text[UI_NTAGS];   /* replaces a tagged text (NULL = as baked) */
    int tag_color_set[UI_NTAGS];
    float tag_color[UI_NTAGS][4];
    int years;
    /* UIGraph: one square per finished day */
    int nsquares;
    int sq_x[UI_MAX_SQUARES], sq_y[UI_MAX_SQUARES];
    float time;  /* for the newest square's flashing */
    float alpha; /* the whole view (1 = opaque) */
} UiState;

typedef struct {
    int kind, graphic, nav[4], nav_mode, action, view, group, interactable, setting, selectme;
    float rect[4];
    /* sliders */
    float vmin, vmax;
    int whole, decimals;
    float tscale;
    const char *postfix;
} UiControlInfo;

int ui_load(void);
int ui_view(const char *name);            /* index, -1 if none */
void ui_state_init(UiState *st, int view); /* tab panels as baked, everything normal */
int ui_button_count(void);
int ui_button_action(int b);
int ui_button_view(int b);
int ui_button_by_action(int view, int action); /* -1 if that view has none */
int ui_button_nav(int b, int dir);        /* explicit links: 0 up, 1 down, 2 left, 3 right; -1 = none */
const UiControlInfo *ui_control(int b);
int ui_group_tab(int g);                  /* a tab panel's index in its UITabView (-1 if not one) */
int ui_group_panel(int g);                /* a game-shown panel's UI_PANEL_* (0 if not one) */
int ui_group_count(void);
int ui_control_group_in(int b, int g);    /* the control is in group g or a group inside it */
float ui_group_clip_height(int g);        /* scrolling content: its viewport's height (0 if not clipped) */
/* scrolling content holding control b: its group, and the scroll that shows the control whole */
int ui_scroll_to_show(int b, const UiState *st, float *scroll);
int ui_group_view(int g);
float ui_group_height(int g);             /* scrolling content's height in pixels */
/* Selectable navigation as Unity does it: explicit links, or the best placed control that can be
 * selected (shown and interactable) in that direction */
int ui_navigate(int view, const UiState *st, int from, int dir);
void ui_draw(int view, const UiState *st, int screen_w, int screen_h);

/* the Vita's button prompts: icons (made at load) and labels in a bar centred on cx, top at y */
enum { UI_ICON_CROSS, UI_ICON_CIRCLE, UI_ICON_TRIANGLE, UI_ICON_SQUARE, UI_ICON_SHOULDER, UI_ICON_DPAD_LR,
       UI_ICON_DPAD_UD, UI_ICON_TOUCH, UI_ICONS };
#define UI_MAX_PROMPTS 8
typedef struct {
    int icon[2];     /* UI_ICON_* (-1 none) */
    char letter[2];  /* on a shoulder icon (L, R) */
    const char *label;
} UiPrompt;
void ui_draw_prompts(const UiPrompt *p, int n, float cx, float y, float alpha, int screen_w, int screen_h);
void ui_prompt_rects(const UiPrompt *p, int n, float cx, float y, float rects[][4]); /* (for touches) */
void ui_draw_label(const char *text, float cx, float cy, float alpha, int screen_w, int screen_h);
/* the touch screen: the control under a point (or whose row it is), -1 if none; a slider's value
 * (0..1) at x; the scrolling content under a point */
int ui_control_at(int view, const UiState *st, float x, float y);
float ui_slider_value_at(int b, float x);
int ui_scroll_group_at(int view, const UiState *st, float x, float y);

#endif
