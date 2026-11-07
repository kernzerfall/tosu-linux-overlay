#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "overlay.h"

static uint8_t *mapping = NULL;
static int mapping_fd = -1;
static uint64_t frame_counter = 0;
static char shm_path[256];

static void channel_unmap(void) {
    if (mapping) {
        munmap(mapping, TOSU_OVERLAY_TOTAL_BYTES);
        mapping = NULL;
    }
    if (mapping_fd >= 0) {
        close(mapping_fd);
        mapping_fd = -1;
    }
}

static void channel_try_map(void) {
    if (!shm_path[0]) {
        const char *override = getenv("TOSU_OVERLAY_SHM");
        if (override && override[0]) {
            snprintf(shm_path, sizeof(shm_path), "%s", override);
        } else {
            snprintf(shm_path, sizeof(shm_path), "/dev/shm/tosu-overlay-%d", (int)getpid());
        }
        tosu_log("waiting for overlay host at %s", shm_path);
    }

    int fd = open(shm_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < TOSU_OVERLAY_TOTAL_BYTES) {
        close(fd);
        return;
    }

    void *ptr = mmap(NULL, TOSU_OVERLAY_TOTAL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        tosu_log("mmap of %s failed", shm_path);
        close(fd);
        return;
    }

    mapping = ptr;
    mapping_fd = fd;

    if (channel_load(TOSU_H_MAGIC) != TOSU_OVERLAY_MAGIC ||
        channel_load(TOSU_H_VERSION) != TOSU_OVERLAY_VERSION) {
        /* host is still initializing (magic is written last) or incompatible */
        channel_unmap();
        return;
    }

    channel_store(TOSU_H_READING_SLOT, TOSU_OVERLAY_NO_SLOT);
    channel_store(TOSU_H_READER_PID, (uint32_t)getpid());
    tosu_log("connected to overlay host (pid %u)", channel_load(TOSU_H_WRITER_PID));
}

void channel_tick(void) {
    frame_counter++;

    if (!mapping) {
        /* opening the file every frame would be wasteful, poll ~once a second */
        if (frame_counter % 60 == 1) {
            channel_try_map();
        }
        return;
    }

    if (frame_counter % 120 == 0) {
        struct stat st;
        /* host removes the file when it exits */
        if (fstat(mapping_fd, &st) != 0 || st.st_nlink == 0) {
            tosu_log("overlay host disconnected");
            channel_unmap();
            return;
        }
    }

    channel_store(TOSU_H_HEARTBEAT, (uint32_t)frame_counter);
}

bool channel_ready(void) { return mapping != NULL; }

uint32_t channel_load(uint32_t offset) {
    return __atomic_load_n((uint32_t *)(mapping + offset), __ATOMIC_SEQ_CST);
}

void channel_store(uint32_t offset, uint32_t value) {
    __atomic_store_n((uint32_t *)(mapping + offset), value, __ATOMIC_SEQ_CST);
}

const uint8_t *channel_slot(uint32_t slot) {
    return mapping + TOSU_OVERLAY_SLOTS_OFFSET + (uint64_t)slot * TOSU_OVERLAY_SLOT_BYTES;
}

void channel_push_event(const tosu_event_t *event) {
    if (!mapping) {
        return;
    }

    uint32_t write = channel_load(TOSU_H_EVENT_WRITE);
    uint32_t read = channel_load(TOSU_H_EVENT_READ);
    if (write - read >= TOSU_OVERLAY_EVENT_COUNT) {
        return; /* host is not draining events, drop */
    }

    uint8_t *dst = mapping + TOSU_OVERLAY_EVENTS_OFFSET +
                   (write % TOSU_OVERLAY_EVENT_COUNT) * TOSU_OVERLAY_EVENT_BYTES;
    memcpy(dst, event, sizeof(*event));
    channel_store(TOSU_H_EVENT_WRITE, write + 1);
}
