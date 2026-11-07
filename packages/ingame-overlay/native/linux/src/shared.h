/*
 * Shared memory layout between the tosu in-game overlay host (Electron, the
 * "writer") and libtosu-overlay.so loaded inside the game (the "reader").
 *
 * The file lives at /dev/shm/tosu-overlay-<game pid>. It is created by the
 * writer and mapped by the reader. All integers are little-endian u32.
 *
 * Keep in sync with packages/ingame-overlay/src/linux/shared.ts
 */
#ifndef TOSU_OVERLAY_SHARED_H
#define TOSU_OVERLAY_SHARED_H

#include <stdint.h>

#define TOSU_OVERLAY_MAGIC 0x4F534F54u /* "TOSO" */
#define TOSU_OVERLAY_VERSION 1u

#define TOSU_OVERLAY_SLOTS 3u
#define TOSU_OVERLAY_MAX_DIM 4096u
#define TOSU_OVERLAY_NO_SLOT 0xFFFFFFFFu
#define TOSU_OVERLAY_MAX_KEYBIND 8u

#define TOSU_OVERLAY_HEADER_BYTES 4096u
#define TOSU_OVERLAY_EVENT_COUNT 256u
#define TOSU_OVERLAY_EVENT_BYTES 64u
#define TOSU_OVERLAY_EVENTS_OFFSET TOSU_OVERLAY_HEADER_BYTES
#define TOSU_OVERLAY_SLOTS_OFFSET                                              \
    (TOSU_OVERLAY_EVENTS_OFFSET + TOSU_OVERLAY_EVENT_COUNT * TOSU_OVERLAY_EVENT_BYTES)
#define TOSU_OVERLAY_SLOT_BYTES                                                \
    ((uint64_t)TOSU_OVERLAY_MAX_DIM * TOSU_OVERLAY_MAX_DIM * 4u)
#define TOSU_OVERLAY_TOTAL_BYTES                                               \
    (TOSU_OVERLAY_SLOTS_OFFSET + TOSU_OVERLAY_SLOTS * TOSU_OVERLAY_SLOT_BYTES)

/* Header byte offsets. */
enum {
    /* written by the overlay host */
    TOSU_H_MAGIC = 0,
    TOSU_H_VERSION = 4,
    TOSU_H_WRITER_PID = 8,
    TOSU_H_SEQ = 16,
    TOSU_H_ACTIVE_SLOT = 20,
    TOSU_H_SLOT_WIDTH = 24,  /* u32[3] */
    TOSU_H_SLOT_HEIGHT = 36, /* u32[3] */
    TOSU_H_VISIBLE = 48,
    TOSU_H_KEYBIND_COUNT = 52,
    TOSU_H_KEYBIND = 56, /* u32[8][2]: SDL scancode alternatives per key */
    TOSU_H_EVENT_READ = 192,

    /* written by the game-side hook */
    TOSU_H_READER_PID = 128,
    TOSU_H_READING_SLOT = 132,
    TOSU_H_GAME_WIDTH = 136,
    TOSU_H_GAME_HEIGHT = 140,
    TOSU_H_CONFIG_MODE = 144,
    TOSU_H_EVENT_WRITE = 148,
    TOSU_H_HEARTBEAT = 152,
};

/* Input events forwarded from the game to the overlay host. */
enum {
    TOSU_EV_KEY = 1,    /* code = SDL scancode, down, mods = SDL keymod */
    TOSU_EV_MOTION = 2, /* x, y in framebuffer pixels */
    TOSU_EV_BUTTON = 3, /* code = SDL button (1 left, 2 middle, 3 right), down, x, y */
    TOSU_EV_WHEEL = 4,  /* dx, dy, x, y */
    TOSU_EV_TEXT = 5,   /* text (utf-8, nul terminated) */
    TOSU_EV_CONFIG = 6, /* down = 1 when configuration mode starts, 0 when it ends */
};

typedef struct {
    uint32_t type;
    uint32_t code;
    uint32_t down;
    uint32_t mods;
    float x;
    float y;
    float dx;
    float dy;
    char text[32];
} tosu_event_t;

_Static_assert(sizeof(tosu_event_t) == TOSU_OVERLAY_EVENT_BYTES, "event size");

#endif
