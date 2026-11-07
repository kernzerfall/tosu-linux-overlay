/*
 * libtosu-overlay.so entry point.
 *
 * Loaded into the game with LD_PRELOAD. osu!lazer (through SDL3 and the .NET
 * runtime) never links EGL/GLX/SDL directly; every function is resolved at
 * runtime with dlsym(). We interpose dlsym itself and hand out our wrappers for
 * the handful of symbols we care about:
 *
 *   eglSwapBuffers / glXSwapBuffers   -> draw the overlay right before present
 *   eglGetProcAddress / glXGetProcAddress(ARB)
 *                                     -> catch swap functions resolved that way
 *   SDL_PollEvent / SDL_PeepEvents    -> keybind + input capture (input.c)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "overlay.h"

/* ---- logging ---- */

static int debug_enabled = -1;

static void vlog(const char *fmt, va_list ap) {
    fputs("[tosu-overlay] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void tosu_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void tosu_debug(const char *fmt, ...) {
    if (debug_enabled < 0) {
        const char *env = getenv("TOSU_OVERLAY_DEBUG");
        debug_enabled = env && env[0] && strcmp(env, "0") != 0;
    }
    if (!debug_enabled) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

/* ---- real dlsym ---- */

typedef void *(*dlsym_fn)(void *, const char *);
static _Atomic(dlsym_fn) real_dlsym_ptr = NULL;

void *tosu_real_dlsym(void *handle, const char *name) {
    dlsym_fn fn = atomic_load(&real_dlsym_ptr);
    if (!fn) {
        /* dlvsym is not interposed, use it to find the real dlsym in libc */
        fn = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!fn) {
            fn = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
        }
        if (!fn) {
            fn = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0");
        }
        if (!fn) {
            tosu_log("unable to find real dlsym, aborting");
            abort();
        }
        atomic_store(&real_dlsym_ptr, fn);
    }
    return fn(handle, name);
}

/* ---- EGL ---- */

typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef unsigned int EGLBoolean;
typedef int EGLint;

#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_DRAW 0x3059

typedef EGLBoolean (*egl_swap_fn)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*egl_swap_damage_fn)(EGLDisplay, EGLSurface, const EGLint *, EGLint);
typedef void *(*egl_getproc_fn)(const char *);
typedef EGLBoolean (*egl_query_surface_fn)(EGLDisplay, EGLSurface, EGLint, EGLint *);
typedef EGLContext (*egl_get_current_context_fn)(void);
typedef EGLSurface (*egl_get_current_surface_fn)(EGLint);

static _Atomic(egl_swap_fn) real_egl_swap = NULL;
static _Atomic(egl_swap_damage_fn) real_egl_swap_damage_khr = NULL;
static _Atomic(egl_swap_damage_fn) real_egl_swap_damage_ext = NULL;
static _Atomic(egl_getproc_fn) real_egl_getproc = NULL;
static _Atomic(void *) egl_handle = NULL;

static void *egl_sym(const char *name) {
    void *handle = atomic_load(&egl_handle);
    void *sym = handle ? tosu_real_dlsym(handle, name) : NULL;
    if (!sym) {
        sym = tosu_real_dlsym(RTLD_DEFAULT, name);
    }
    if (!sym) {
        egl_getproc_fn getproc = atomic_load(&real_egl_getproc);
        if (getproc) {
            sym = getproc(name);
        }
    }
    return sym;
}

static void *egl_gl_getproc(const char *name) {
    egl_getproc_fn getproc = atomic_load(&real_egl_getproc);
    if (!getproc) {
        getproc = (egl_getproc_fn)egl_sym("eglGetProcAddress");
        atomic_store(&real_egl_getproc, getproc);
    }
    return getproc ? getproc(name) : NULL;
}

static void egl_before_present(EGLDisplay dpy, EGLSurface surface) {
    static egl_query_surface_fn query_surface;
    static egl_get_current_context_fn get_context;
    static egl_get_current_surface_fn get_surface;

    if (!query_surface) {
        query_surface = (egl_query_surface_fn)egl_sym("eglQuerySurface");
        get_context = (egl_get_current_context_fn)egl_sym("eglGetCurrentContext");
        get_surface = (egl_get_current_surface_fn)egl_sym("eglGetCurrentSurface");
        if (!query_surface || !get_context || !get_surface) {
            return;
        }
    }

    channel_tick();
    if (!channel_ready()) {
        return;
    }

    EGLContext context = get_context();
    /* only draw when the presented surface is the one bound to the context */
    if (!context || get_surface(EGL_DRAW) != surface) {
        return;
    }

    EGLint width = 0, height = 0;
    if (!query_surface(dpy, surface, EGL_WIDTH, &width) ||
        !query_surface(dpy, surface, EGL_HEIGHT, &height)) {
        return;
    }

    gl_render_frame(context, egl_gl_getproc, width, height);
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    egl_before_present(dpy, surface);
    return atomic_load(&real_egl_swap)(dpy, surface);
}

static EGLBoolean hook_eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface,
                                                   const EGLint *rects, EGLint n) {
    egl_before_present(dpy, surface);
    /* the overlay may touch the whole surface, damage everything */
    return atomic_load(&real_egl_swap_damage_khr)(dpy, surface, rects, 0);
}

static EGLBoolean hook_eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surface,
                                                   const EGLint *rects, EGLint n) {
    egl_before_present(dpy, surface);
    return atomic_load(&real_egl_swap_damage_ext)(dpy, surface, rects, 0);
}

static void *hook_egl_symbol(const char *name, void *real);

static void *hook_eglGetProcAddress(const char *name) {
    void *real = atomic_load(&real_egl_getproc)(name);
    void *hooked = name ? hook_egl_symbol(name, real) : NULL;
    return hooked ? hooked : real;
}

/* Returns our replacement for an EGL symbol, storing the real one. */
static void *hook_egl_symbol(const char *name, void *real) {
    if (!real || strncmp(name, "egl", 3) != 0) {
        return NULL;
    }

    if (strcmp(name, "eglSwapBuffers") == 0) {
        atomic_store(&real_egl_swap, (egl_swap_fn)real);
        return (void *)hook_eglSwapBuffers;
    }
    if (strcmp(name, "eglSwapBuffersWithDamageKHR") == 0) {
        atomic_store(&real_egl_swap_damage_khr, (egl_swap_damage_fn)real);
        return (void *)hook_eglSwapBuffersWithDamageKHR;
    }
    if (strcmp(name, "eglSwapBuffersWithDamageEXT") == 0) {
        atomic_store(&real_egl_swap_damage_ext, (egl_swap_damage_fn)real);
        return (void *)hook_eglSwapBuffersWithDamageEXT;
    }
    if (strcmp(name, "eglGetProcAddress") == 0) {
        atomic_store(&real_egl_getproc, (egl_getproc_fn)real);
        return (void *)hook_eglGetProcAddress;
    }
    return NULL;
}

/* ---- GLX ---- */

typedef struct _XDisplay Display;
typedef unsigned long GLXDrawable;
typedef void *GLXContext;

#define GLX_WIDTH 0x801D
#define GLX_HEIGHT 0x801E

typedef void (*glx_swap_fn)(Display *, GLXDrawable);
typedef void *(*glx_getproc_fn)(const unsigned char *);
typedef void (*glx_query_drawable_fn)(Display *, GLXDrawable, int, unsigned int *);
typedef GLXContext (*glx_get_current_context_fn)(void);
typedef GLXDrawable (*glx_get_current_drawable_fn)(void);

static _Atomic(glx_swap_fn) real_glx_swap = NULL;
static _Atomic(glx_getproc_fn) real_glx_getproc = NULL;
static _Atomic(glx_getproc_fn) real_glx_getproc_arb = NULL;
static _Atomic(void *) glx_handle = NULL;

static void *glx_sym(const char *name) {
    void *handle = atomic_load(&glx_handle);
    void *sym = handle ? tosu_real_dlsym(handle, name) : NULL;
    return sym ? sym : tosu_real_dlsym(RTLD_DEFAULT, name);
}

static void *glx_gl_getproc(const char *name) {
    glx_getproc_fn getproc = atomic_load(&real_glx_getproc_arb);
    if (!getproc) {
        getproc = atomic_load(&real_glx_getproc);
    }
    if (!getproc) {
        getproc = (glx_getproc_fn)glx_sym("glXGetProcAddressARB");
        atomic_store(&real_glx_getproc_arb, getproc);
    }
    return getproc ? getproc((const unsigned char *)name) : NULL;
}

static void hook_glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    static glx_query_drawable_fn query_drawable;
    static glx_get_current_context_fn get_context;
    static glx_get_current_drawable_fn get_drawable;

    if (!query_drawable) {
        query_drawable = (glx_query_drawable_fn)glx_sym("glXQueryDrawable");
        get_context = (glx_get_current_context_fn)glx_sym("glXGetCurrentContext");
        get_drawable = (glx_get_current_drawable_fn)glx_sym("glXGetCurrentDrawable");
    }

    if (query_drawable && get_context && get_drawable) {
        channel_tick();
        GLXContext context = get_context();
        if (channel_ready() && context && get_drawable() == drawable) {
            unsigned int width = 0, height = 0;
            query_drawable(dpy, drawable, GLX_WIDTH, &width);
            query_drawable(dpy, drawable, GLX_HEIGHT, &height);
            gl_render_frame(context, glx_gl_getproc, (int)width, (int)height);
        }
    }

    atomic_load(&real_glx_swap)(dpy, drawable);
}

static void *hook_glx_symbol(const char *name, void *real);

static void *hook_glXGetProcAddress(const unsigned char *name) {
    void *real = atomic_load(&real_glx_getproc)(name);
    void *hooked = name ? hook_glx_symbol((const char *)name, real) : NULL;
    return hooked ? hooked : real;
}

static void *hook_glXGetProcAddressARB(const unsigned char *name) {
    void *real = atomic_load(&real_glx_getproc_arb)(name);
    void *hooked = name ? hook_glx_symbol((const char *)name, real) : NULL;
    return hooked ? hooked : real;
}

static void *hook_glx_symbol(const char *name, void *real) {
    if (!real || strncmp(name, "glX", 3) != 0) {
        return NULL;
    }

    if (strcmp(name, "glXSwapBuffers") == 0) {
        atomic_store(&real_glx_swap, (glx_swap_fn)real);
        return (void *)hook_glXSwapBuffers;
    }
    if (strcmp(name, "glXGetProcAddress") == 0) {
        atomic_store(&real_glx_getproc, (glx_getproc_fn)real);
        return (void *)hook_glXGetProcAddress;
    }
    if (strcmp(name, "glXGetProcAddressARB") == 0) {
        atomic_store(&real_glx_getproc_arb, (glx_getproc_fn)real);
        return (void *)hook_glXGetProcAddressARB;
    }
    return NULL;
}

/* ---- dlsym interposer ---- */

__attribute__((visibility("default"))) void *dlsym(void *handle, const char *name) {
    void *real = tosu_real_dlsym(handle, name);
    if (!real) {
        return real;
    }

    void *hooked = NULL;
    if (name[0] == 'e') {
        hooked = hook_egl_symbol(name, real);
        if (hooked && handle != RTLD_DEFAULT && handle != RTLD_NEXT) {
            atomic_store(&egl_handle, handle);
        }
    } else if (name[0] == 'g') {
        hooked = hook_glx_symbol(name, real);
        if (hooked && handle != RTLD_DEFAULT && handle != RTLD_NEXT) {
            atomic_store(&glx_handle, handle);
        }
    } else if (name[0] == 'S') {
        hooked = input_hook_symbol(handle, name, real);
    }

    return hooked ? hooked : real;
}

__attribute__((constructor)) static void tosu_overlay_init(void) {
    tosu_debug("loaded into pid %d", (int)getpid());
}
