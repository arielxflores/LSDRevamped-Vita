/* Scripted input, shared by the PC test harness and the Vita benchmark mode.
 *
 * Script lines:
 *   wait N                 N frames with no input
 *   hold N btn... lx=..    N frames holding buttons / stick values; tx=X ty=Y: a finger on the touch
 *                          screen there (screen pixels)
 *   cmd <text>             game_command(text)
 *   shot <name>            capture the next frame to <name>.png
 *   log <text>             write text to the log
 *   quit                   end the run
 */
#include "common.h"

static char *g_buf, *g_cur;
static int g_active, g_quit;
static int g_hold;
static uint32_t g_btn, g_prev;
static float g_lx, g_ly, g_rx, g_ry;
static int g_touch;
static float g_tx, g_ty;
static char g_shot[128];

int script_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 1 << 20) {
        fclose(f);
        return 0;
    }
    g_buf = (char *)calloc(1, (size_t)n + 1);
    size_t got = fread(g_buf, 1, (size_t)n, f);
    fclose(f);
    g_buf[got] = 0;
    g_cur = g_buf;
    g_active = 1;
    lsd_log("script: running %s (%u bytes)", path, (unsigned)got);
    return 1;
}

static int next_line(char *line, size_t cap)
{
    if (!g_cur || !*g_cur)
        return 0;
    size_t i = 0;
    while (*g_cur && *g_cur != '\n') {
        if (i + 1 < cap) line[i++] = *g_cur;
        g_cur++;
    }
    if (*g_cur == '\n') g_cur++;
    line[i] = 0;
    return 1;
}

int script_active(void) { return g_active; }

static uint32_t parse_button(const char *s)
{
    static const struct { const char *n; uint32_t b; } map[] = {
        {"up", BTN_UP}, {"down", BTN_DOWN}, {"left", BTN_LEFT}, {"right", BTN_RIGHT},
        {"cross", BTN_CROSS}, {"circle", BTN_CIRCLE}, {"square", BTN_SQUARE}, {"triangle", BTN_TRIANGLE},
        {"l", BTN_L}, {"r", BTN_R}, {"start", BTN_START}, {"select", BTN_SELECT}};
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(s, map[i].n)) return map[i].b;
    lsd_log("script: unknown button %s", s);
    return 0;
}

static void step(void)
{
    char line[512];
    while (g_hold <= 0 && !g_quit) {
        if (!next_line(line, sizeof(line))) {
            g_quit = 1;
            return;
        }
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#')
            continue;
        g_btn = 0;
        g_lx = g_ly = g_rx = g_ry = 0;
        g_touch = 0;
        if (!strncmp(line, "cmd ", 4)) {
            game_command(line + 4);
        } else if (!strncmp(line, "shot ", 5)) {
            snprintf(g_shot, sizeof(g_shot), "%.*s", (int)sizeof(g_shot) - 1, line + 5);
            g_hold = 1;
        } else if (!strncmp(line, "log ", 4)) {
            lsd_log("script: %s", line + 4);
        } else if (!strncmp(line, "quit", 4)) {
            g_quit = 1;
        } else if (!strncmp(line, "wait ", 5) || !strncmp(line, "hold ", 5)) {
            char *tok = strtok(line + 5, " ");
            g_hold = tok ? atoi(tok) : 1;
            if (g_hold < 1) g_hold = 1;
            while ((tok = strtok(NULL, " "))) {
                if (!strncmp(tok, "lx=", 3)) g_lx = (float)atof(tok + 3);
                else if (!strncmp(tok, "ly=", 3)) g_ly = (float)atof(tok + 3);
                else if (!strncmp(tok, "rx=", 3)) g_rx = (float)atof(tok + 3);
                else if (!strncmp(tok, "ry=", 3)) g_ry = (float)atof(tok + 3);
                else if (!strncmp(tok, "tx=", 3)) g_tx = (float)atof(tok + 3), g_touch = 1;
                else if (!strncmp(tok, "ty=", 3)) g_ty = (float)atof(tok + 3), g_touch = 1;
                else g_btn |= parse_button(tok);
            }
        } else {
            lsd_log("script: bad line: %s", line);
        }
    }
}

int script_input(Input *in)
{
    step();
    if (g_quit) {
        lsd_log("script: finished");
        return 0;
    }
    memset(in, 0, sizeof(*in));
    in->held = g_btn;
    in->pressed = g_btn & ~g_prev;
    g_prev = g_btn;
    in->lx = g_lx;
    in->ly = g_ly;
    in->rx = g_rx;
    in->ry = g_ry;
    in->touch = g_touch;
    in->tx = g_tx;
    in->ty = g_ty;
    g_hold--;
    return 1;
}

const char *script_take_shot(void)
{
    static char name[128];
    if (!g_shot[0])
        return NULL;
    snprintf(name, sizeof(name), "%s", g_shot);
    g_shot[0] = 0;
    return name;
}
