/* LSD Revamped's Lua API (MoonSharp in the original) on Lua 5.2, for the dream entities. */
#ifndef LSD_LUAAPI_H
#define LSD_LUAAPI_H

#include "scene.h"

int luaapi_dream_begin(void);       /* after scene_load: set up scripts, runners and the player */
void luaapi_dream_end(void);        /* free every script state of the dream */

void luaapi_entity_init(int comp);  /* InteractiveObject.Init / TriggerLua.Init */
void luaapi_interactive_enable(int comp);
void luaapi_interactive_update(int comp, float dt);
void luaapi_runner_update_comp(int comp);
void luaapi_player_runner_update(void);
void luaapi_trigger_lua(int comp, int enter);

/* DreamAudio (sound arrives in a later build; state is tracked now) */
void luaapi_audio_enable(int comp, int on);
void luaapi_log_init_times(void);
void luaapi_perftest(void);
void luaapi_audio_update(int comp, float dt);

/* player control from scripts (PlayerMovement / PlayerRotation external input) */
typedef struct {
    int move_external;
    float move_x, move_y;
    int rot_external;
    float rot;
} PlayerExternal;
extern PlayerExternal g_player_ext;

/* DreamSystem.OnDreamTimeout callbacks */
void luaapi_dream_timeout(void);

int luaapi_errors(void);
void luaapi_tick(float dt);           /* Time.time / Time.deltaTime for scripts */

#endif
