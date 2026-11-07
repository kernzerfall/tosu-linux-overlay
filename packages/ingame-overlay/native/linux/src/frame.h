#ifndef TOSU_OVERLAY_FRAME_H
#define TOSU_OVERLAY_FRAME_H

#include <stdbool.h>
#include <stdint.h>

/* A BGRA, premultiplied, top-down, tightly packed overlay frame. */
typedef struct {
    const uint8_t *pixels;
    uint32_t width;
    uint32_t height;
} frame_view_t;

/*
 * If the host published a frame newer than *last_seq, pin its slot and return
 * it. The slot stays valid until the next successful frame_acquire().
 */
bool frame_acquire(uint32_t *last_seq, frame_view_t *out);

/* Host wants the overlay shown. */
bool frame_visible(void);

/* Tell the host the size of the game's framebuffer in pixels. */
void frame_report_size(int width, int height);

#endif
