/*
 * Input handling through SDL3 (osu!lazer resolves SDL with dlsym, see hook.c).
 *
 * osu!framework handles mouse events (and keyboard events while no text box is
 * focused) inside an SDL_SetEventFilter callback and drops them from the queue,
 * everything else arrives through SDL_PeepEvents. Both paths are hooked; an
 * event seen by both is only processed once.
 *
 * - Watches key events for the configuration keybind (scancodes come from the
 *   host through the shared header).
 * - While configuration mode is active, keyboard / mouse / text events are
 *   removed from the game's event stream and forwarded to the host instead.
 * - Key and button releases are always delivered to the game when the game saw
 *   the matching press, so nothing gets stuck.
 *
 * SDL_Event layouts are part of SDL3's stable ABI; offsets were taken from SDL 3.4
 * headers and are verified by tests/sdl_abi_check.c.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <string.h>

#include "overlay.h"

#define SDL_EVENT_SIZE 128

#define SDL_EVENT_WINDOW_FOCUS_LOST 0x20F
#define SDL_EVENT_KEY_DOWN 0x300
#define SDL_EVENT_KEY_UP 0x301
#define SDL_EVENT_TEXT_EDITING 0x302
#define SDL_EVENT_TEXT_INPUT 0x303
#define SDL_EVENT_TEXT_EDITING_CANDIDATES 0x307
#define SDL_EVENT_MOUSE_MOTION 0x400
#define SDL_EVENT_MOUSE_BUTTON_DOWN 0x401
#define SDL_EVENT_MOUSE_BUTTON_UP 0x402
#define SDL_EVENT_MOUSE_WHEEL 0x403

#define SDL_GETEVENT 2
#define SDL_MOUSEWHEEL_FLIPPED 1

/* field offsets inside SDL_Event */
#define OFF_TYPE 0
#define OFF_TIMESTAMP 8
#define OFF_WINDOW_ID 16
#define OFF_KEY_SCANCODE 24
#define OFF_KEY_MOD 32
#define OFF_KEY_DOWN 36
#define OFF_KEY_REPEAT 37
#define OFF_MOTION_X 28
#define OFF_MOTION_Y 32
#define OFF_MOTION_XREL 36
#define OFF_MOTION_YREL 40
#define OFF_BUTTON_BUTTON 24
#define OFF_BUTTON_DOWN 25
#define OFF_BUTTON_X 28
#define OFF_BUTTON_Y 32
#define OFF_WHEEL_X 24
#define OFF_WHEEL_Y 28
#define OFF_WHEEL_DIRECTION 32
#define OFF_WHEEL_MOUSE_X 36
#define OFF_WHEEL_MOUSE_Y 40
#define OFF_TEXT_TEXT 24

typedef bool (*sdl_poll_event_fn)(void *event);
typedef int (*sdl_peep_events_fn)(void *events, int count, int action, uint32_t min,
                                  uint32_t max);
typedef void *(*sdl_get_window_from_id_fn)(uint32_t id);
typedef float (*sdl_get_pixel_density_fn)(void *window);
typedef bool (*sdl_window_bool_fn)(void *window);
typedef bool (*sdl_set_relative_fn)(void *window, bool enabled);
typedef bool (*sdl_event_filter_fn)(void *userdata, void *event);
typedef void (*sdl_set_event_filter_fn)(sdl_event_filter_fn filter, void *userdata);
typedef bool (*sdl_get_event_filter_fn)(sdl_event_filter_fn *filter, void **userdata);

static _Atomic(sdl_poll_event_fn) real_poll_event = NULL;
static _Atomic(sdl_peep_events_fn) real_peep_events = NULL;
static _Atomic(sdl_set_event_filter_fn) real_set_event_filter = NULL;
static _Atomic(sdl_get_event_filter_fn) real_get_event_filter = NULL;

/* the game's own event filter, called by ours */
static _Atomic(sdl_event_filter_fn) app_filter = NULL;
static _Atomic(void *) app_filter_userdata = NULL;
static _Atomic(void *) sdl_handle = NULL;

static struct {
    bool loaded;
    sdl_get_window_from_id_fn get_window_from_id;
    sdl_get_pixel_density_fn get_pixel_density;
    sdl_window_bool_fn get_relative;
    sdl_set_relative_fn set_relative;
    sdl_window_bool_fn text_input_active;
    sdl_window_bool_fn start_text_input;
    sdl_window_bool_fn stop_text_input;
} sdl;

/* game thread (event polling) state */
static bool config_mode = false;
static bool keys_down[512];        /* physical state */
static bool game_saw_key[512];     /* game received the press */
static bool game_saw_button[16];
static uint32_t last_window_id = 0;
static bool restore_relative = false;
static bool restore_text_input = false;
static float config_x = 0.0f; /* cursor tracked from relative motion, pixels */
static float config_y = 0.0f;

/* read by the render thread */
static _Atomic bool cursor_active = false;
static _Atomic float cursor_x = 0.0f;
static _Atomic float cursor_y = 0.0f;

void input_cursor(cursor_state_t *out) {
    out->active = atomic_load(&cursor_active);
    out->x = atomic_load(&cursor_x);
    out->y = atomic_load(&cursor_y);
}

static uint32_t ev_u32(const uint8_t *ev, int off) {
    uint32_t v;
    memcpy(&v, ev + off, 4);
    return v;
}
static float ev_f32(const uint8_t *ev, int off) {
    float v;
    memcpy(&v, ev + off, 4);
    return v;
}
static uint16_t ev_u16(const uint8_t *ev, int off) {
    uint16_t v;
    memcpy(&v, ev + off, 2);
    return v;
}

static void load_sdl(void) {
    if (sdl.loaded) {
        return;
    }
    void *handle = atomic_load(&sdl_handle);
    if (!handle) {
        handle = RTLD_DEFAULT;
    }
    sdl.get_window_from_id = (sdl_get_window_from_id_fn)tosu_real_dlsym(handle, "SDL_GetWindowFromID");
    sdl.get_pixel_density = (sdl_get_pixel_density_fn)tosu_real_dlsym(handle, "SDL_GetWindowPixelDensity");
    sdl.get_relative = (sdl_window_bool_fn)tosu_real_dlsym(handle, "SDL_GetWindowRelativeMouseMode");
    sdl.set_relative = (sdl_set_relative_fn)tosu_real_dlsym(handle, "SDL_SetWindowRelativeMouseMode");
    sdl.text_input_active = (sdl_window_bool_fn)tosu_real_dlsym(handle, "SDL_TextInputActive");
    sdl.start_text_input = (sdl_window_bool_fn)tosu_real_dlsym(handle, "SDL_StartTextInput");
    sdl.stop_text_input = (sdl_window_bool_fn)tosu_real_dlsym(handle, "SDL_StopTextInput");
    sdl.loaded = true;
}

static void *window_for(uint32_t id) {
    load_sdl();
    return sdl.get_window_from_id && id ? sdl.get_window_from_id(id) : NULL;
}

/* SDL mouse coordinates are in window points, the overlay works in pixels */
static float pixel_density(uint32_t window_id) {
    void *window = window_for(window_id);
    float density = window && sdl.get_pixel_density ? sdl.get_pixel_density(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

static void set_config_mode(bool enabled) {
    if (enabled == config_mode) {
        return;
    }
    config_mode = enabled;
    if (enabled) {
        config_x = (float)channel_load(TOSU_H_GAME_WIDTH) / 2.0f;
        config_y = (float)channel_load(TOSU_H_GAME_HEIGHT) / 2.0f;
        atomic_store(&cursor_x, config_x);
        atomic_store(&cursor_y, config_y);
    }
    atomic_store(&cursor_active, enabled);

    void *window = window_for(last_window_id);
    if (window) {
        if (enabled) {
            /* osu! uses relative mouse mode; the overlay needs an absolute cursor */
            restore_relative = sdl.get_relative && sdl.get_relative(window);
            if (restore_relative && sdl.set_relative) {
                sdl.set_relative(window, false);
            }
            restore_text_input = sdl.text_input_active && sdl.text_input_active(window);
            if (!restore_text_input && sdl.start_text_input) {
                sdl.start_text_input(window);
            }
        } else {
            if (restore_relative && sdl.set_relative) {
                sdl.set_relative(window, true);
            }
            if (!restore_text_input && sdl.stop_text_input) {
                sdl.stop_text_input(window);
            }
        }
    }

    if (channel_ready()) {
        channel_store(TOSU_H_CONFIG_MODE, enabled ? 1 : 0);
        tosu_event_t ev = {.type = TOSU_EV_CONFIG, .down = enabled ? 1 : 0};
        channel_push_event(&ev);
    }
    tosu_log("configuration mode %s", enabled ? "started" : "ended");
}

static bool keybind_contains(uint32_t scancode, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = channel_load(TOSU_H_KEYBIND + i * 8);
        uint32_t b = channel_load(TOSU_H_KEYBIND + i * 8 + 4);
        if (scancode == a || (b && scancode == b)) {
            return true;
        }
    }
    return false;
}

static bool keybind_pressed(uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = channel_load(TOSU_H_KEYBIND + i * 8);
        uint32_t b = channel_load(TOSU_H_KEYBIND + i * 8 + 4);
        bool down = (a < 512 && keys_down[a]) || (b && b < 512 && keys_down[b]);
        if (!down) {
            return false;
        }
    }
    return count > 0;
}

/* Returns true if the event must be hidden from the game. */
static bool filter_event(const uint8_t *ev) {
    uint32_t type = ev_u32(ev, OFF_TYPE);
    if (type < SDL_EVENT_WINDOW_FOCUS_LOST || type > SDL_EVENT_MOUSE_WHEEL) {
        return false;
    }

    bool connected = channel_ready();
    if (!connected && !config_mode) {
        return false;
    }
    if (!connected && config_mode) {
        set_config_mode(false);
    }

    uint32_t window_id = ev_u32(ev, OFF_WINDOW_ID);
    if (window_id) {
        last_window_id = window_id;
    }

    switch (type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        set_config_mode(false);
        return false;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        uint32_t scancode = ev_u32(ev, OFF_KEY_SCANCODE);
        bool down = ev[OFF_KEY_DOWN] != 0;
        bool repeat = ev[OFF_KEY_REPEAT] != 0;
        if (scancode >= 512) {
            return config_mode;
        }

        if (!down) {
            keys_down[scancode] = false;
            if (config_mode) {
                tosu_event_t out = {.type = TOSU_EV_KEY, .code = scancode, .down = 0,
                                    .mods = ev_u16(ev, OFF_KEY_MOD)};
                channel_push_event(&out);
            }
            /* release goes to the game iff it saw the press */
            if (game_saw_key[scancode]) {
                game_saw_key[scancode] = false;
                return false;
            }
            return true;
        }

        if (!repeat) {
            keys_down[scancode] = true;
            uint32_t count = connected ? channel_load(TOSU_H_KEYBIND_COUNT) : 0;
            if (count > TOSU_OVERLAY_MAX_KEYBIND) {
                count = TOSU_OVERLAY_MAX_KEYBIND;
            }
            if (keybind_contains(scancode, count) && keybind_pressed(count)) {
                set_config_mode(!config_mode);
                return true;
            }
        }

        if (config_mode) {
            tosu_event_t out = {.type = TOSU_EV_KEY, .code = scancode, .down = 1,
                                .mods = ev_u16(ev, OFF_KEY_MOD)};
            channel_push_event(&out);
            return true;
        }

        game_saw_key[scancode] = true;
        return false;
    }

    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_EDITING_CANDIDATES:
        return config_mode;

    case SDL_EVENT_TEXT_INPUT: {
        if (!config_mode) {
            return false;
        }
        const char *text;
        memcpy(&text, ev + OFF_TEXT_TEXT, sizeof(text));
        if (text) {
            tosu_event_t out = {.type = TOSU_EV_TEXT};
            strncpy(out.text, text, sizeof(out.text) - 1);
            channel_push_event(&out);
        }
        return true;
    }

    case SDL_EVENT_MOUSE_MOTION: {
        if (!config_mode) {
            return false;
        }
        float density = pixel_density(window_id);
        void *window = window_for(window_id);
        if (window && sdl.get_relative && sdl.get_relative(window)) {
            /* relative mode (e.g. re-enabled by the game): positions are frozen,
             * integrate the deltas ourselves */
            config_x += ev_f32(ev, OFF_MOTION_XREL) * density;
            config_y += ev_f32(ev, OFF_MOTION_YREL) * density;
        } else {
            config_x = ev_f32(ev, OFF_MOTION_X) * density;
            config_y = ev_f32(ev, OFF_MOTION_Y) * density;
        }
        float max_x = (float)channel_load(TOSU_H_GAME_WIDTH) - 1.0f;
        float max_y = (float)channel_load(TOSU_H_GAME_HEIGHT) - 1.0f;
        if (config_x < 0.0f) config_x = 0.0f;
        if (config_y < 0.0f) config_y = 0.0f;
        if (max_x > 0.0f && config_x > max_x) config_x = max_x;
        if (max_y > 0.0f && config_y > max_y) config_y = max_y;

        atomic_store(&cursor_x, config_x);
        atomic_store(&cursor_y, config_y);
        tosu_event_t out = {.type = TOSU_EV_MOTION, .x = config_x, .y = config_y};
        channel_push_event(&out);
        return true;
    }

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        uint32_t button = ev[OFF_BUTTON_BUTTON];
        bool down = ev[OFF_BUTTON_DOWN] != 0;
        if (button >= 16) {
            return config_mode;
        }

        if (config_mode) {
            tosu_event_t out = {.type = TOSU_EV_BUTTON, .code = button, .down = down,
                                .x = config_x, .y = config_y};
            channel_push_event(&out);
        }

        if (!down) {
            if (game_saw_button[button]) {
                game_saw_button[button] = false;
                return false;
            }
            return true;
        }
        if (config_mode) {
            return true;
        }
        game_saw_button[button] = true;
        return false;
    }

    case SDL_EVENT_MOUSE_WHEEL: {
        if (!config_mode) {
            return false;
        }
        float flip = ev_u32(ev, OFF_WHEEL_DIRECTION) == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        tosu_event_t out = {.type = TOSU_EV_WHEEL,
                            .dx = ev_f32(ev, OFF_WHEEL_X) * flip,
                            .dy = ev_f32(ev, OFF_WHEEL_Y) * flip,
                            .x = config_x,
                            .y = config_y};
        channel_push_event(&out);
        return true;
    }

    default:
        return false;
    }
}

/*
 * An event can pass the game's event filter and then be queued, reaching us twice.
 * Remember recent decisions keyed by the event's leading bytes (type, timestamp,
 * window, key/button fields) so side effects (keybind toggle, forwarding) run once.
 */
#define RECENT_EVENTS 64
static struct {
    uint64_t hash;
    bool swallow;
} recent[RECENT_EVENTS];
static unsigned recent_next = 0;

static uint64_t event_hash(const uint8_t *ev) {
    uint64_t h = 1469598103934665603ull; /* FNV-1a */
    for (int i = 0; i < 48; i++) {
        h = (h ^ ev[i]) * 1099511628211ull;
    }
    return h | 1; /* never 0, 0 marks an empty slot */
}

static bool filter_event_once(const uint8_t *ev) {
    uint32_t type = ev_u32(ev, OFF_TYPE);
    if (type < SDL_EVENT_WINDOW_FOCUS_LOST || type > SDL_EVENT_MOUSE_WHEEL) {
        return false;
    }

    uint64_t hash = event_hash(ev);
    for (unsigned i = 0; i < RECENT_EVENTS; i++) {
        if (recent[i].hash == hash) {
            return recent[i].swallow;
        }
    }

    bool swallow = filter_event(ev);
    recent[recent_next].hash = hash;
    recent[recent_next].swallow = swallow;
    recent_next = (recent_next + 1) % RECENT_EVENTS;
    return swallow;
}

static bool hook_event_filter(void *userdata, void *event) {
    if (event && filter_event_once(event)) {
        return false; /* captured by the overlay, drop before the game sees it */
    }
    sdl_event_filter_fn filter = atomic_load(&app_filter);
    return filter ? filter(atomic_load(&app_filter_userdata), event) : true;
}

static void hook_SDL_SetEventFilter(sdl_event_filter_fn filter, void *userdata) {
    atomic_store(&app_filter, filter);
    atomic_store(&app_filter_userdata, userdata);
    /* always keep ours installed so input is seen before the game's filter */
    atomic_load(&real_set_event_filter)(hook_event_filter, NULL);
}

static bool hook_SDL_GetEventFilter(sdl_event_filter_fn *filter, void **userdata) {
    sdl_event_filter_fn current = atomic_load(&app_filter);
    if (filter) *filter = current;
    if (userdata) *userdata = atomic_load(&app_filter_userdata);
    return current != NULL;
}

static bool hook_SDL_PollEvent(void *event) {
    sdl_poll_event_fn poll = atomic_load(&real_poll_event);
    for (;;) {
        if (!poll(event)) {
            return false;
        }
        /* SDL_PollEvent(NULL) only checks for pending events */
        if (!event || !filter_event_once(event)) {
            return true;
        }
    }
}

static int hook_SDL_PeepEvents(void *events, int count, int action, uint32_t min, uint32_t max) {
    int result = atomic_load(&real_peep_events)(events, count, action, min, max);
    if (result <= 0 || action != SDL_GETEVENT || !events) {
        return result;
    }

    uint8_t *base = events;
    int kept = 0;
    for (int i = 0; i < result; i++) {
        uint8_t *ev = base + (size_t)i * SDL_EVENT_SIZE;
        if (filter_event_once(ev)) {
            continue;
        }
        if (kept != i) {
            memmove(base + (size_t)kept * SDL_EVENT_SIZE, ev, SDL_EVENT_SIZE);
        }
        kept++;
    }
    return kept;
}

void *input_hook_symbol(void *handle, const char *name, void *real) {
    if (strncmp(name, "SDL_", 4) != 0) {
        return NULL;
    }

    void *hooked;
    if (strcmp(name, "SDL_PollEvent") == 0) {
        atomic_store(&real_poll_event, (sdl_poll_event_fn)real);
        hooked = (void *)hook_SDL_PollEvent;
    } else if (strcmp(name, "SDL_PeepEvents") == 0) {
        atomic_store(&real_peep_events, (sdl_peep_events_fn)real);
        hooked = (void *)hook_SDL_PeepEvents;
    } else if (strcmp(name, "SDL_SetEventFilter") == 0) {
        atomic_store(&real_set_event_filter, (sdl_set_event_filter_fn)real);
        hooked = (void *)hook_SDL_SetEventFilter;
    } else if (strcmp(name, "SDL_GetEventFilter") == 0) {
        atomic_store(&real_get_event_filter, (sdl_get_event_filter_fn)real);
        hooked = (void *)hook_SDL_GetEventFilter;
    } else {
        return NULL;
    }

    if (handle != RTLD_DEFAULT && handle != RTLD_NEXT) {
        atomic_store(&sdl_handle, handle);
    }
    return hooked;
}
