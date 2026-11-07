/*
 * OpenGL / OpenGL ES 3 overlay renderer, called right before the game swaps.
 * Saves and restores every piece of GL state it touches.
 */
#include <stddef.h>
#include <string.h>

#include "frame.h"
#include "overlay.h"

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef float GLfloat;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLE_STRIP 0x0005
#define GL_ONE 1
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_FUNC_ADD 0x8006
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_VIEWPORT 0x0BA2
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_VERSION 0x1F02
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_ALPHA 0x80CA
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_EQUATION_RGB 0x8009
#define GL_BLEND_EQUATION_ALPHA 0x883D
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_RGBA8 0x8058
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_PIXEL_UNPACK_BUFFER_BINDING 0x88EF
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_SAMPLER_BINDING 0x8919
#define GL_FRAMEBUFFER_SRGB 0x8DB9

#define GL_FUNCS(X)                                                                        \
    X(void, GetIntegerv, GLenum, GLint *)                                                  \
    X(void, GetBooleanv, GLenum, GLboolean *)                                              \
    X(GLboolean, IsEnabled, GLenum)                                                        \
    X(void, Enable, GLenum)                                                                \
    X(void, Disable, GLenum)                                                               \
    X(void, BlendFuncSeparate, GLenum, GLenum, GLenum, GLenum)                             \
    X(void, BlendEquationSeparate, GLenum, GLenum)                                         \
    X(void, ColorMask, GLboolean, GLboolean, GLboolean, GLboolean)                         \
    X(void, Viewport, GLint, GLint, GLsizei, GLsizei)                                      \
    X(void, UseProgram, GLuint)                                                            \
    X(void, BindVertexArray, GLuint)                                                       \
    X(void, GenVertexArrays, GLsizei, GLuint *)                                            \
    X(void, BindBuffer, GLenum, GLuint)                                                    \
    X(void, ActiveTexture, GLenum)                                                         \
    X(void, BindTexture, GLenum, GLuint)                                                   \
    X(void, GenTextures, GLsizei, GLuint *)                                                \
    X(void, TexParameteri, GLenum, GLenum, GLint)                                          \
    X(void, TexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,     \
      const void *)                                                                        \
    X(void, TexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,  \
      const void *)                                                                        \
    X(void, PixelStorei, GLenum, GLint)                                                    \
    X(void, BindFramebuffer, GLenum, GLuint)                                               \
    X(GLuint, CreateShader, GLenum)                                                        \
    X(void, ShaderSource, GLuint, GLsizei, const GLchar *const *, const GLint *)           \
    X(void, CompileShader, GLuint)                                                         \
    X(void, GetShaderiv, GLuint, GLenum, GLint *)                                          \
    X(void, GetShaderInfoLog, GLuint, GLsizei, GLsizei *, GLchar *)                        \
    X(GLuint, CreateProgram, void)                                                         \
    X(void, AttachShader, GLuint, GLuint)                                                  \
    X(void, LinkProgram, GLuint)                                                           \
    X(void, GetProgramiv, GLuint, GLenum, GLint *)                                         \
    X(void, GetProgramInfoLog, GLuint, GLsizei, GLsizei *, GLchar *)                       \
    X(void, DeleteShader, GLuint)                                                          \
    X(GLint, GetUniformLocation, GLuint, const GLchar *)                                   \
    X(void, Uniform1i, GLint, GLint)                                                       \
    X(void, Uniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat)                          \
    X(void, DrawArrays, GLenum, GLint, GLsizei)                                            \
    X(const unsigned char *, GetString, GLenum)

typedef struct {
#define X(ret, name, ...) ret (*name)(__VA_ARGS__);
    GL_FUNCS(X)
#undef X
    void (*BindSampler)(GLuint, GLuint); /* optional, GL 3.3 / ES 3.0 */
} gl_api_t;

typedef struct {
    void *context;
    bool failed;
    bool es;
    gl_api_t gl;
    GLuint program;
    GLuint vao;
    GLuint texture;
    GLint u_rect;
    GLint u_mode;
    GLint u_tex;
    uint32_t tex_width;
    uint32_t tex_height;
    uint32_t last_seq;
    bool has_frame;
} gl_state_t;

static gl_state_t state;

static const char *vs_body =
    "out vec2 uv;\n"
    "uniform vec4 u_rect;\n"
    "void main() {\n"
    "    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
    "    uv = vec2(p.x, 1.0 - p.y);\n"
    "    gl_Position = vec4(mix(u_rect.xy, u_rect.zw, p), 0.0, 1.0);\n"
    "}\n";

static const char *fs_body =
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_mode;\n"
    "void main() {\n"
    "    if (u_mode == 0) {\n"
    /* chromium bitmaps are BGRA, uploaded as RGBA */
    "        color = texture(u_tex, uv).bgra;\n"
    "    } else {\n"
    /* configuration mode cursor: white dot with dark outline */
    "        float d = length(uv - 0.5) * 2.0;\n"
    "        float outer = 1.0 - smoothstep(0.85, 1.0, d);\n"
    "        float inner = 1.0 - smoothstep(0.55, 0.7, d);\n"
    "        color = vec4(vec3(inner), 1.0) * outer;\n"
    "    }\n"
    "}\n";

static GLuint compile(GLenum type, const char *body) {
    gl_api_t *gl = &state.gl;
    const char *sources[3] = {
        state.es ? "#version 300 es\n" : "#version 150\n",
        state.es ? "precision mediump float;\n" : "",
        body,
    };

    GLuint shader = gl->CreateShader(type);
    gl->ShaderSource(shader, 3, sources, NULL);
    gl->CompileShader(shader);

    GLint ok = 0;
    gl->GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        gl->GetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
        tosu_log("gl: shader compile failed: %s", log);
        gl->DeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool init_context(void *context, gl_getproc_fn getproc) {
    memset(&state, 0, sizeof(state));
    state.context = context;

    gl_api_t *gl = &state.gl;
#define X(ret, name, ...)                                                                  \
    gl->name = (ret(*)(__VA_ARGS__))getproc("gl" #name);                                   \
    if (!gl->name) {                                                                       \
        tosu_log("gl: missing gl" #name);                                                  \
        return false;                                                                      \
    }
    GL_FUNCS(X)
#undef X
    gl->BindSampler = (void (*)(GLuint, GLuint))getproc("glBindSampler");

    const char *version = (const char *)gl->GetString(GL_VERSION);
    state.es = version && strstr(version, "OpenGL ES") != NULL;
    tosu_log("gl: attaching to context %p (%s)", context, version ? version : "?");

    /* compiling changes no bound state, but the program binding is restored by the caller */
    GLuint vs = compile(GL_VERTEX_SHADER, vs_body);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_body);
    if (!vs || !fs) {
        return false;
    }

    state.program = gl->CreateProgram();
    gl->AttachShader(state.program, vs);
    gl->AttachShader(state.program, fs);
    gl->LinkProgram(state.program);
    gl->DeleteShader(vs);
    gl->DeleteShader(fs);

    GLint ok = 0;
    gl->GetProgramiv(state.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        gl->GetProgramInfoLog(state.program, sizeof(log) - 1, NULL, log);
        tosu_log("gl: program link failed: %s", log);
        return false;
    }

    state.u_rect = gl->GetUniformLocation(state.program, "u_rect");
    state.u_mode = gl->GetUniformLocation(state.program, "u_mode");
    state.u_tex = gl->GetUniformLocation(state.program, "u_tex");

    gl->GenVertexArrays(1, &state.vao);
    gl->GenTextures(1, &state.texture);
    return true;
}

typedef struct {
    GLint program, vao, active_texture, texture, sampler, draw_fb, viewport[4];
    GLint unpack_buffer, unpack_alignment, unpack_row_length, unpack_skip_rows,
        unpack_skip_pixels;
    GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    GLint blend_eq_rgb, blend_eq_alpha;
    GLboolean color_mask[4];
    GLboolean blend, depth, stencil, scissor, cull, srgb;
} saved_state_t;

static void save_state(saved_state_t *s) {
    gl_api_t *gl = &state.gl;
    gl->GetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    gl->GetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    gl->GetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture);
    s->sampler = 0;
    if (gl->BindSampler) {
        gl->GetIntegerv(GL_SAMPLER_BINDING, &s->sampler);
    }
    gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fb);
    gl->GetIntegerv(GL_VIEWPORT, s->viewport);
    gl->GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s->unpack_buffer);
    gl->GetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack_alignment);
    gl->GetIntegerv(GL_UNPACK_ROW_LENGTH, &s->unpack_row_length);
    gl->GetIntegerv(GL_UNPACK_SKIP_ROWS, &s->unpack_skip_rows);
    gl->GetIntegerv(GL_UNPACK_SKIP_PIXELS, &s->unpack_skip_pixels);
    gl->GetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
    gl->GetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
    gl->GetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_alpha);
    gl->GetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_alpha);
    gl->GetIntegerv(GL_BLEND_EQUATION_RGB, &s->blend_eq_rgb);
    gl->GetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blend_eq_alpha);
    gl->GetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
    s->blend = gl->IsEnabled(GL_BLEND);
    s->depth = gl->IsEnabled(GL_DEPTH_TEST);
    s->stencil = gl->IsEnabled(GL_STENCIL_TEST);
    s->scissor = gl->IsEnabled(GL_SCISSOR_TEST);
    s->cull = gl->IsEnabled(GL_CULL_FACE);
    s->srgb = state.es ? GL_FALSE : gl->IsEnabled(GL_FRAMEBUFFER_SRGB);
}

static void set_cap(GLenum cap, GLboolean enabled) {
    if (enabled) {
        state.gl.Enable(cap);
    } else {
        state.gl.Disable(cap);
    }
}

static void restore_state(const saved_state_t *s) {
    gl_api_t *gl = &state.gl;
    gl->UseProgram((GLuint)s->program);
    gl->BindVertexArray((GLuint)s->vao);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_2D, (GLuint)s->texture);
    if (gl->BindSampler) {
        gl->BindSampler(0, (GLuint)s->sampler);
    }
    gl->ActiveTexture((GLenum)s->active_texture);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->draw_fb);
    gl->Viewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)s->unpack_buffer);
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, s->unpack_alignment);
    gl->PixelStorei(GL_UNPACK_ROW_LENGTH, s->unpack_row_length);
    gl->PixelStorei(GL_UNPACK_SKIP_ROWS, s->unpack_skip_rows);
    gl->PixelStorei(GL_UNPACK_SKIP_PIXELS, s->unpack_skip_pixels);
    gl->BlendFuncSeparate((GLenum)s->blend_src_rgb, (GLenum)s->blend_dst_rgb,
                          (GLenum)s->blend_src_alpha, (GLenum)s->blend_dst_alpha);
    gl->BlendEquationSeparate((GLenum)s->blend_eq_rgb, (GLenum)s->blend_eq_alpha);
    gl->ColorMask(s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3]);
    set_cap(GL_BLEND, s->blend);
    set_cap(GL_DEPTH_TEST, s->depth);
    set_cap(GL_STENCIL_TEST, s->stencil);
    set_cap(GL_SCISSOR_TEST, s->scissor);
    set_cap(GL_CULL_FACE, s->cull);
    if (!state.es) {
        set_cap(GL_FRAMEBUFFER_SRGB, s->srgb);
    }
}

static void upload(const frame_view_t *frame) {
    gl_api_t *gl = &state.gl;
    gl->BindTexture(GL_TEXTURE_2D, state.texture);
    gl->BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl->PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    gl->PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);

    if (frame->width != state.tex_width || frame->height != state.tex_height) {
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)frame->width,
                       (GLsizei)frame->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame->pixels);
        state.tex_width = frame->width;
        state.tex_height = frame->height;
    } else {
        gl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)frame->width,
                          (GLsizei)frame->height, GL_RGBA, GL_UNSIGNED_BYTE, frame->pixels);
    }
    state.has_frame = true;
}

void gl_render_frame(void *context, gl_getproc_fn getproc, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }

    if (context != state.context) {
        if (!init_context(context, getproc)) {
            state.failed = true;
            tosu_log("gl: overlay disabled for this context");
        }
    }
    if (state.failed) {
        return;
    }

    frame_report_size(width, height);

    cursor_state_t cursor;
    input_cursor(&cursor);

    frame_view_t frame;
    bool has_new = frame_acquire(&state.last_seq, &frame);
    bool visible = frame_visible();
    if (!has_new && (!visible || (!state.has_frame && !cursor.active))) {
        return;
    }

    gl_api_t *gl = &state.gl;
    saved_state_t saved;
    save_state(&saved);

    if (has_new) {
        upload(&frame);
    }

    if (visible) {
        gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        gl->Viewport(0, 0, width, height);
        gl->Disable(GL_DEPTH_TEST);
        gl->Disable(GL_STENCIL_TEST);
        gl->Disable(GL_SCISSOR_TEST);
        gl->Disable(GL_CULL_FACE);
        if (!state.es) {
            gl->Disable(GL_FRAMEBUFFER_SRGB);
        }
        gl->Enable(GL_BLEND);
        gl->BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        gl->BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        gl->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        gl->UseProgram(state.program);
        gl->BindVertexArray(state.vao);
        gl->ActiveTexture(GL_TEXTURE0);
        gl->BindTexture(GL_TEXTURE_2D, state.texture);
        if (gl->BindSampler) {
            gl->BindSampler(0, 0);
        }
        gl->Uniform1i(state.u_tex, 0);

        if (state.has_frame) {
            gl->Uniform1i(state.u_mode, 0);
            gl->Uniform4f(state.u_rect, -1.0f, -1.0f, 1.0f, 1.0f);
            gl->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        if (cursor.active) {
            float r = 10.0f;
            float x0 = (cursor.x - r) / (float)width * 2.0f - 1.0f;
            float x1 = (cursor.x + r) / (float)width * 2.0f - 1.0f;
            float y0 = 1.0f - (cursor.y + r) / (float)height * 2.0f;
            float y1 = 1.0f - (cursor.y - r) / (float)height * 2.0f;
            gl->Uniform1i(state.u_mode, 1);
            gl->Uniform4f(state.u_rect, x0, y0, x1, y1);
            gl->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }

    restore_state(&saved);
}
