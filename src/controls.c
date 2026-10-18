/* LSDR's control schemes and Game actions on the Vita's pad (see controls.h) */
#include <ctype.h>
#include <math.h>
#include "controls.h"
#include "settings.h"

static const char *const ACTION_NAMES[NUM_REBINDABLE] = {"Move", "Interact", "Run", "LookUp", "LookDown",
                                                         "NextProfile", "Strafe"};

/* InputActions.inputactions' Gamepad bindings, on the Vita: buttonSouth Cross, buttonEast Circle,
 * buttonNorth Triangle, buttonWest Square, select Select, the triggers L and R */
static const Binding DEFAULTS[NUM_REBINDABLE][MAX_BINDINGS] = {
    {{{CTL_LSTICK, 0}}, {{CTL_DPAD, 0}}},
    {{{CTL_CIRCLE, 0}}, {{0, 0}}},
    {{{CTL_CROSS, 0}}, {{0, 0}}},
    {{{CTL_TRIANGLE, 0}}, {{0, 0}}},
    {{{CTL_SQUARE, 0}}, {{0, 0}}},
    {{{CTL_SELECT, 0}}, {{0, 0}}},
    {{{CTL_L, CTL_R}}, {{0, 0}}},
};
static const int NBIND[NUM_REBINDABLE] = {2, 1, 1, 1, 1, 1, 1};

/* InputControl display names (DontUseShortDisplayNames, title case), as the Vita's pad has them */
static const char *const CTL_NAMES[NUM_CTLS] = {
    "", "Cross", "Circle", "Square", "Triangle", "L", "R", "Start", "Select",
    "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
    "Left Stick Up", "Left Stick Down", "Left Stick Left", "Left Stick Right",
    "Right Stick Up", "Right Stick Down", "Right Stick Left", "Right Stick Right",
    "D-Pad", "Left Stick", "Right Stick"};
/* (in schemes.txt) */
static const char *const CTL_IDS[NUM_CTLS] = {
    "none", "cross", "circle", "square", "triangle", "l", "r", "start", "select",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "ls_up", "ls_down", "ls_left", "ls_right",
    "rs_up", "rs_down", "rs_left", "rs_right", "dpad", "lstick", "rstick"};

static ControlScheme g_s[MAX_SCHEMES];
static int g_n;
static int g_use_file = 1;
static char g_path[256];

static void defaults_into(ControlScheme *s)
{
    memcpy(s->b, DEFAULTS, sizeof(DEFAULTS));
}

/* the order the dropdown has them in (List<string>.Sort: case first ignored, then lower before upper) */
static int name_cmp(const char *a, const char *b)
{
    for (const char *p = a, *q = b;; p++, q++) {
        int x = tolower((unsigned char)*p), y = tolower((unsigned char)*q);
        if (x != y || !x) {
            if (x != y) return x - y;
            break;
        }
    }
    return -strcmp(a, b);
}

static void sort_schemes(void)
{
    for (int i = 1; i < g_n; i++)
        for (int j = i; j > 0 && name_cmp(g_s[j - 1].name, g_s[j].name) > 0; j--) {
            ControlScheme t = g_s[j];
            g_s[j] = g_s[j - 1];
            g_s[j - 1] = t;
        }
}

static int ctl_from_id(const char *id)
{
    for (int i = 0; i < NUM_CTLS; i++)
        if (!strcmp(CTL_IDS[i], id))
            return i;
    return CTL_NONE;
}

static void load(void)
{
    FILE *f = fopen(g_path, "r");
    if (!f)
        return;
    char line[160];
    ControlScheme cur;
    int in = 0, nload = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "scheme ", 7)) {
            memset(&cur, 0, sizeof(cur));
            snprintf(cur.name, sizeof(cur.name), "%.31s", line + 7);
            cur.sensitivity = 15.0f;
            cur.editable = 1;
            defaults_into(&cur);
            in = 1;
            continue;
        }
        if (!in)
            continue;
        int a, i;
        float v;
        char c0[24], c1[24];
        if (sscanf(line, "fps %d", &a) == 1) {
            cur.fps = a != 0;
        } else if (sscanf(line, "sensitivity %f", &v) == 1) {
            cur.sensitivity = v >= 1 && v <= 30 ? v : 15.0f;
        } else if (sscanf(line, "invert %d", &a) == 1) {
            cur.invert_y = a != 0;
        } else if (sscanf(line, "bind %d %d %23s %23s", &a, &i, c0, c1) == 4 && a >= 0 && a < NUM_REBINDABLE &&
                   i >= 0 && i < NBIND[a]) {
            cur.b[a][i].ctl[0] = (uint8_t)ctl_from_id(c0);
            cur.b[a][i].ctl[1] = (uint8_t)(a == ACT_STRAFE ? ctl_from_id(c1) : CTL_NONE);
        } else if (!strcmp(line, "end")) {
            in = 0;
            int dup = 0;
            for (int k = 0; k < g_n; k++)
                if (!strcmp(g_s[k].name, cur.name))
                    dup = 1;
            if (!dup && cur.name[0] && g_n < MAX_SCHEMES) {
                g_s[g_n++] = cur;
                nload++;
            }
        }
    }
    fclose(f);
    lsd_log("control schemes: %d of your own (%s)", nload, g_path);
}

void controls_save(void)
{
    if (!g_use_file) {
        lsd_log("control schemes: (test run: not saved)");
        return;
    }
    char tmp[272];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        lsd_log("control schemes: cannot write %s", tmp);
        return;
    }
    for (int k = 0; k < g_n; k++) {
        const ControlScheme *s = &g_s[k];
        if (!s->editable)
            continue; /* (the built-in ones are made again at every start) */
        fprintf(f, "scheme %s\nfps %d\nsensitivity %g\ninvert %d\n", s->name, s->fps, s->sensitivity, s->invert_y);
        for (int a = 0; a < NUM_REBINDABLE; a++)
            for (int i = 0; i < NBIND[a]; i++)
                fprintf(f, "bind %d %d %s %s\n", a, i, CTL_IDS[s->b[a][i].ctl[0] % NUM_CTLS],
                        CTL_IDS[s->b[a][i].ctl[1] % NUM_CTLS]);
        fprintf(f, "end\n");
    }
    int ok = fclose(f) == 0;
    remove(g_path);
    if (!ok || rename(tmp, g_path) != 0)
        lsd_log("control schemes: saving %s failed", g_path);
    else
        lsd_log("control schemes: saved");
}

void controls_init(int use_file)
{
    g_use_file = use_file;
    snprintf(g_path, sizeof(g_path), "%s/schemes.txt", plat_write_dir());
    memset(g_s, 0, sizeof(g_s));
    g_n = 0;
    /* ControlSchemeLoaderSystem.EnsureDefaultSchemes */
    static const char *const NAMES[2] = {"Classic", "Revamped"};
    for (int k = 0; k < 2; k++) {
        ControlScheme *s = &g_s[g_n++];
        snprintf(s->name, sizeof(s->name), "%s", NAMES[k]);
        s->fps = k == 1;
        s->sensitivity = 15.0f;
        s->invert_y = 0;
        s->editable = 0;
        defaults_into(s);
    }
    if (use_file)
        load();
    sort_schemes();
}

int controls_count(void) { return g_n; }
const ControlScheme *controls_scheme(int i) { return &g_s[i >= 0 && i < g_n ? i : 0]; }
ControlScheme *controls_scheme_mut(int i) { return &g_s[i >= 0 && i < g_n ? i : 0]; }
int controls_current_index(void) { return S.scheme >= 0 && S.scheme < g_n ? S.scheme : 0; }
const ControlScheme *controls_current(void) { return &g_s[controls_current_index()]; }

void controls_select(int i)
{
    S.scheme = i >= 0 && i < g_n ? i : 0;
}

int controls_create(const ControlScheme *s)
{
    if (g_n >= MAX_SCHEMES)
        return -1;
    g_s[g_n++] = *s;
    sort_schemes();
    for (int k = 0; k < g_n; k++)
        if (!strcmp(g_s[k].name, s->name)) {
            controls_select(k);
            lsd_log("control scheme %s made (%s controls, sensitivity %.1f%s)", s->name, s->fps ? "FPS" : "tank",
                    s->sensitivity, s->invert_y ? ", look inverted" : "");
            return k;
        }
    return -1;
}

void controls_delete(int i)
{
    if (i < 0 || i >= g_n || !g_s[i].editable)
        return;
    lsd_log("control scheme %s deleted", g_s[i].name);
    memmove(&g_s[i], &g_s[i + 1], sizeof(ControlScheme) * (size_t)(g_n - i - 1));
    g_n--;
    int cur = S.scheme;
    if (cur > i) cur--;
    if (cur >= g_n) cur = g_n - 1;
    controls_select(cur);
}

int controls_name_valid(const char *name, int can_have_same)
{
    int blank = 1;
    for (const char *p = name; *p; p++)
        if (!isspace((unsigned char)*p))
            blank = 0;
    if (blank)
        return 0;
    if (!can_have_same)
        for (int k = 0; k < g_n; k++)
            if (!strcmp(g_s[k].name, name))
                return 0;
    return 1;
}

void controls_new_from(ControlScheme *out, const ControlScheme *from)
{
    memset(out, 0, sizeof(*out));
    out->fps = from->fps;
    out->sensitivity = from->sensitivity;
    out->invert_y = from->invert_y;
    out->editable = 1;
    defaults_into(out); /* (a new scheme's binding overrides start empty) */
}

int controls_bindings(int action) { return action >= 0 && action < NUM_REBINDABLE ? NBIND[action] : 0; }
int controls_composite(int action) { return action == ACT_STRAFE; }
int controls_expects_vector(int action) { return action == ACT_MOVE; }
const char *controls_action_name(int action) { return action >= 0 && action < NUM_REBINDABLE ? ACTION_NAMES[action] : ""; }
Binding controls_default(int action, int idx) { return DEFAULTS[action][idx]; }
const char *controls_ctl_name(int ctl) { return ctl > 0 && ctl < NUM_CTLS ? CTL_NAMES[ctl] : ""; }

/* GetBindingDisplayStringNotEmpty */
void controls_binding_text(const ControlScheme *s, int action, int idx, char *buf, size_t n)
{
    const Binding *b = &s->b[action][idx];
    if (controls_composite(action)) {
        if (!b->ctl[0] && !b->ctl[1])
            snprintf(buf, n, "<unbound>");
        else
            snprintf(buf, n, "%s/%s", controls_ctl_name(b->ctl[0]), controls_ctl_name(b->ctl[1]));
    } else {
        snprintf(buf, n, "%s", b->ctl[0] ? controls_ctl_name(b->ctl[0]) : "<unbound>");
    }
}

/* UIRebindContainerPopulator.getActionDisplayString */
void controls_row_text(const ControlScheme *s, int action, char *buf, size_t n)
{
    size_t len = (size_t)snprintf(buf, n, "%s (", controls_action_name(action));
    for (int i = 0; i < NBIND[action] && len < n; i++) {
        char t[48];
        controls_binding_text(s, action, i, t, sizeof(t));
        len += (size_t)snprintf(buf + len, n - len, "%s%s", i ? ", " : "", t);
    }
    if (len < n)
        snprintf(buf + len, n - len, ")");
}

/* ---------------------------------------------------------------- the actions, each frame */

static const Input *g_in;
static uint8_t g_held[NUM_REBINDABLE], g_was[NUM_REBINDABLE];

static float dz(float v) { return fabsf(v) < 0.35f ? 0.0f : v; } /* (the game's stick dead zone) */

static float ctl_value(const Input *in, int c)
{
    switch (c) {
    case CTL_CROSS: return (in->held & BTN_CROSS) ? 1.0f : 0.0f;
    case CTL_CIRCLE: return (in->held & BTN_CIRCLE) ? 1.0f : 0.0f;
    case CTL_SQUARE: return (in->held & BTN_SQUARE) ? 1.0f : 0.0f;
    case CTL_TRIANGLE: return (in->held & BTN_TRIANGLE) ? 1.0f : 0.0f;
    case CTL_L: return (in->held & BTN_L) ? 1.0f : 0.0f;
    case CTL_R: return (in->held & BTN_R) ? 1.0f : 0.0f;
    case CTL_START: return (in->held & BTN_START) ? 1.0f : 0.0f;
    case CTL_SELECT: return (in->held & BTN_SELECT) ? 1.0f : 0.0f;
    case CTL_DPAD_UP: return (in->held & BTN_UP) ? 1.0f : 0.0f;
    case CTL_DPAD_DOWN: return (in->held & BTN_DOWN) ? 1.0f : 0.0f;
    case CTL_DPAD_LEFT: return (in->held & BTN_LEFT) ? 1.0f : 0.0f;
    case CTL_DPAD_RIGHT: return (in->held & BTN_RIGHT) ? 1.0f : 0.0f;
    case CTL_LS_UP: return in->ly < 0 ? -in->ly : 0.0f;
    case CTL_LS_DOWN: return in->ly > 0 ? in->ly : 0.0f;
    case CTL_LS_LEFT: return in->lx < 0 ? -in->lx : 0.0f;
    case CTL_LS_RIGHT: return in->lx > 0 ? in->lx : 0.0f;
    case CTL_RS_UP: return in->ry < 0 ? -in->ry : 0.0f;
    case CTL_RS_DOWN: return in->ry > 0 ? in->ry : 0.0f;
    case CTL_RS_LEFT: return in->rx < 0 ? -in->rx : 0.0f;
    case CTL_RS_RIGHT: return in->rx > 0 ? in->rx : 0.0f;
    case CTL_DPAD: return (in->held & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)) ? 1.0f : 0.0f;
    case CTL_LSTICK: return sqrtf(in->lx * in->lx + in->ly * in->ly);
    case CTL_RSTICK: return sqrtf(in->rx * in->rx + in->ry * in->ry);
    default: return 0.0f;
    }
}

/* a 2D control's vector (y up) */
static void ctl_vector(const Input *in, int c, float *x, float *y)
{
    *x = *y = 0;
    if (c == CTL_LSTICK) {
        *x = dz(in->lx);
        *y = -dz(in->ly);
    } else if (c == CTL_RSTICK) {
        *x = dz(in->rx);
        *y = -dz(in->ry);
    } else if (c == CTL_DPAD) {
        *x = ((in->held & BTN_RIGHT) ? 1.0f : 0.0f) - ((in->held & BTN_LEFT) ? 1.0f : 0.0f);
        *y = ((in->held & BTN_UP) ? 1.0f : 0.0f) - ((in->held & BTN_DOWN) ? 1.0f : 0.0f);
    }
}

void controls_update(const Input *in)
{
    g_in = in;
    const ControlScheme *s = controls_current();
    for (int a = 0; a < NUM_REBINDABLE; a++) {
        g_was[a] = g_held[a];
        int h = 0;
        for (int i = 0; i < NBIND[a]; i++)
            for (int k = 0; k < (a == ACT_STRAFE ? 2 : 1); k++)
                if (s->b[a][i].ctl[k] && ctl_value(in, s->b[a][i].ctl[k]) >= 0.5f) /* (the press point) */
                    h = 1;
        if (a == ACT_MOVE) {
            float x, y;
            ctl_move(&x, &y);
            h = x != 0 || y != 0;
        }
        g_held[a] = (uint8_t)h;
    }
}

int ctl_held(int action) { return action >= 0 && action < NUM_REBINDABLE && g_held[action]; }
int ctl_pressed(int action) { return action >= 0 && action < NUM_REBINDABLE && g_held[action] && !g_was[action]; }
int ctl_released(int action) { return action >= 0 && action < NUM_REBINDABLE && !g_held[action] && g_was[action]; }

void ctl_move(float *x, float *y)
{
    *x = *y = 0;
    if (!g_in)
        return;
    const ControlScheme *s = controls_current();
    float best = 0;
    /* (a Value action with several bindings: the one actuated the most drives it) */
    for (int i = 0; i < NBIND[ACT_MOVE]; i++) {
        float vx, vy;
        ctl_vector(g_in, s->b[ACT_MOVE][i].ctl[0], &vx, &vy);
        float m = vx * vx + vy * vy;
        if (m > best) {
            best = m;
            *x = vx;
            *y = vy;
        }
    }
}

void ctl_look(float *x, float *y)
{
    *x = *y = 0;
    if (!g_in)
        return;
    ctl_vector(g_in, CTL_RSTICK, x, y);
    if (controls_current()->invert_y) /* invertVector2(invertY=true) */
        *y = -*y;
}

float ctl_strafe(void)
{
    if (!g_in)
        return 0;
    const Binding *b = &controls_current()->b[ACT_STRAFE][0];
    float v = 0;
    if (b->ctl[0] && ctl_value(g_in, b->ctl[0]) >= 0.5f) v -= 1;
    if (b->ctl[1] && ctl_value(g_in, b->ctl[1]) >= 0.5f) v += 1;
    return v;
}

/* ---------------------------------------------------------------- interactive rebinding */

static struct {
    int active, vector, armed;
} CAP;

void controls_capture_begin(int vector)
{
    CAP.active = 1;
    CAP.vector = vector;
    CAP.armed = 0;
}

int controls_capture_poll(const Input *in)
{
    if (!CAP.active)
        return -1;
    int any = in->held != 0 || fabsf(in->lx) > 0.35f || fabsf(in->ly) > 0.35f || fabsf(in->rx) > 0.35f ||
              fabsf(in->ry) > 0.35f;
    if (!CAP.armed) { /* (the press that started it, let go first) */
        if (!any)
            CAP.armed = 1;
        return -2;
    }
    if (in->held & BTN_SELECT) { /* WithCancelingThrough: the CancelBinding action's Select */
        CAP.active = 0;
        return -1;
    }
    int got = -2;
    if (CAP.vector) {
        if (ctl_value(in, CTL_DPAD) > 0) got = CTL_DPAD;
        else if (ctl_value(in, CTL_LSTICK) > 0.7f) got = CTL_LSTICK;
        else if (ctl_value(in, CTL_RSTICK) > 0.7f) got = CTL_RSTICK;
    } else {
        for (int c = CTL_CROSS; c <= CTL_RS_RIGHT && got < 0; c++)
            if (c != CTL_SELECT && ctl_value(in, c) > (c >= CTL_LS_UP ? 0.7f : 0.5f))
                got = c;
    }
    if (got >= 0)
        CAP.active = 0;
    return got;
}

/* ---------------------------------------------------------------- tests */

int controls_command(const char *cmd)
{
    if (!strcmp(cmd, "schemes")) {
        for (int k = 0; k < g_n; k++) {
            char rows[512];
            size_t len = 0;
            rows[0] = 0;
            for (int a = 0; a < NUM_REBINDABLE && len < sizeof(rows); a++) {
                char t[96];
                controls_row_text(&g_s[k], a, t, sizeof(t));
                len += (size_t)snprintf(rows + len, sizeof(rows) - len, "%s%s", a ? ", " : "", t);
            }
            lsd_log("scheme %d%s %s: %s, sensitivity %.2f%s%s; %s", k, k == controls_current_index() ? "*" : "",
                    g_s[k].name, g_s[k].fps ? "FPS" : "tank", g_s[k].sensitivity, g_s[k].invert_y ? ", inverted" : "",
                    g_s[k].editable ? "" : " (built in)", rows);
        }
        return 1;
    }
    return 0;
}
