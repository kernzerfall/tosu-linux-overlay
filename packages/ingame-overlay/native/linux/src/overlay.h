#ifndef TOSU_OVERLAY_H
#define TOSU_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "shared.h"

void tosu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void tosu_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Real dlsym, bypassing our interposer. */
void *tosu_real_dlsym(void *handle, const char *name);

/* ---- channel.c: shared memory with the overlay host ---- */

/* Called once per presented frame. Maps / unmaps the shared file as needed. */
void channel_tick(void);
bool channel_ready(void);
uint32_t channel_load(uint32_t offset);
void channel_store(uint32_t offset, uint32_t value);
/* Pointer to the start of a frame slot. */
const uint8_t *channel_slot(uint32_t slot);
/* Queue an input event for the overlay host. Drops it if the ring is full. */
void channel_push_event(const tosu_event_t *event);

/* ---- input.c: SDL3 event filtering ---- */

typedef struct {
    bool active;
    float x;
    float y;
} cursor_state_t;

void input_cursor(cursor_state_t *out);
void *input_hook_symbol(void *handle, const char *name, void *real);

/* ---- render_gl.c: OpenGL drawing ---- */

typedef void *(*gl_getproc_fn)(const char *name);

/*
 * Draw the overlay into the current context's default framebuffer.
 * context: opaque current context handle, used to detect context changes.
 */
void gl_render_frame(void *context, gl_getproc_fn getproc, int width, int height);

#endif
