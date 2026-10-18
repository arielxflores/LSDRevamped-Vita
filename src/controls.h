/* LSDR's control schemes (ControlScheme, ControlSchemeLoaderSystem) and the Game actions of its
 * InputActions (Move, Look, Interact, Run, LookUp, LookDown, Pause, NextProfile, Strafe) with the
 * pad bindings the Vita has, rebindable per scheme (RebindableActions, IndexedActionBinding,
 * UIRebinder's interactive rebinding). Schemes the player makes are kept in
 * ux0:data/lsdr/schemes.txt. */
#ifndef LSD_CONTROLS_H
#define LSD_CONTROLS_H

#include "common.h"

/* the rebindable actions in RebindableActions' order (the Game map without Look and Pause) */
enum { ACT_MOVE, ACT_INTERACT, ACT_RUN, ACT_LOOK_UP, ACT_LOOK_DOWN, ACT_NEXT_PROFILE, ACT_STRAFE, NUM_REBINDABLE };

#define MAX_BINDINGS 2  /* an action's pad bindings (Move: the left stick and the d-pad) */
#define MAX_SCHEMES 16
#define SCHEME_NAME_MAX 32

/* what a binding can be bound to: a button (a stick's direction counts as one), or a whole stick / d-pad */
enum {
    CTL_NONE, CTL_CROSS, CTL_CIRCLE, CTL_SQUARE, CTL_TRIANGLE, CTL_L, CTL_R, CTL_START, CTL_SELECT,
    CTL_DPAD_UP, CTL_DPAD_DOWN, CTL_DPAD_LEFT, CTL_DPAD_RIGHT, CTL_LS_UP, CTL_LS_DOWN, CTL_LS_LEFT, CTL_LS_RIGHT,
    CTL_RS_UP, CTL_RS_DOWN, CTL_RS_LEFT, CTL_RS_RIGHT, CTL_DPAD, CTL_LSTICK, CTL_RSTICK, NUM_CTLS
};

typedef struct {
    uint8_t ctl[2]; /* a composite's parts (Strafe: negative, positive); otherwise ctl[0] */
} Binding;

typedef struct {
    char name[SCHEME_NAME_MAX];
    int fps;           /* FpsControls */
    float sensitivity; /* MouseSensitivity */
    int invert_y;      /* InvertLookY */
    int editable;      /* the built-in Classic and Revamped are not */
    Binding b[NUM_REBINDABLE][MAX_BINDINGS];
} ControlScheme;

void controls_init(int use_file);   /* the built-in schemes, then the saved ones (0: test runs, never saved) */
int controls_count(void);
const ControlScheme *controls_scheme(int i);
ControlScheme *controls_scheme_mut(int i);
const ControlScheme *controls_current(void); /* GameSettings.CurrentControlSchemeIndex's scheme */
int controls_current_index(void);
void controls_select(int i);                 /* ControlSchemeLoaderSystem.SelectScheme */
int controls_create(const ControlScheme *s); /* CreateScheme(select: true); its index, -1 if full */
void controls_delete(int i);                 /* DeleteScheme */
void controls_save(void);                    /* SaveSchemes */
int controls_name_valid(const char *name, int can_have_same); /* UIControlSchemeNameVerifier.Validate */
void controls_new_from(ControlScheme *out, const ControlScheme *from); /* new ControlScheme(other) */

/* bindings */
int controls_bindings(int action);          /* how many (Move 2, the others 1) */
int controls_composite(int action);         /* Strafe */
int controls_expects_vector(int action);    /* Move: a stick or the d-pad */
const char *controls_action_name(int action);
Binding controls_default(int action, int idx);
const char *controls_ctl_name(int ctl);
void controls_binding_text(const ControlScheme *s, int action, int idx, char *buf, size_t n);
void controls_row_text(const ControlScheme *s, int action, char *buf, size_t n); /* "Run (Cross)" */

/* each frame, the current scheme's actions from the pad */
void controls_update(const Input *in);
int ctl_held(int action);
int ctl_pressed(int action);
int ctl_released(int action);
void ctl_move(float *x, float *y); /* Move: the strongest of its bindings (StickDeadzone) */
void ctl_look(float *x, float *y); /* Look: the right stick, its Y inverted with InvertLookY */
float ctl_strafe(void);            /* Strafe: -1, 0, 1 */

/* interactive rebinding: waits for everything to be let go, then the first control of the kind
 * wanted (Select cancels) */
void controls_capture_begin(int vector);
int controls_capture_poll(const Input *in); /* -2 still waiting, -1 cancelled, else the CTL_ */

int controls_command(const char *cmd); /* test commands; 1 if handled */

#endif
