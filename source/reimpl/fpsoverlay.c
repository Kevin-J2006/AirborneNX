#include "fpsoverlay.h"
#include "../utils/init.h"
#include "../utils/logger.h"
#include <GLES3/gl3.h>
#include <switch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Frame rate overlay
//
// Status Monitor and the other overlays only count the frames of retail
// games, so the port measures its own: the time between one presented frame
// and the next, taken where the game calls eglSwapBuffers.
//
// The panel is drawn into the finished frame with the game's own context, as
// one batch of coloured rectangles (the text is a 5x7 bitmap font made of
// them). Everything it changes in the context is put back afterwards, because
// the engine remembers the GL state it has set and does not set it again; the
// vertex layout lives in a vertex array object of its own for the same
// reason.
// ============================================================================
#define GRAPH_FRAMES   120      // frames in the graph, one bar each
#define GRAPH_TOP_MS   50.0f    // frame time at the top of the graph
#define WINDOW_MS      500.0f   // how often the figures are refreshed
#define MAX_QUADS      640
#define GOOD_MS        20.0f    // bars up to here are green (60 FPS)
#define FAIR_MS        36.0f    // and up to here yellow (30 FPS)

typedef struct {
    float x, y;
    uint8_t r, g, b, a;
} vertex;

typedef struct {
    GLint program, vertex_array, array_buffer, framebuffer;
    GLint viewport[4];
    GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    GLint blend_eq_rgb, blend_eq_alpha;
    GLboolean color_mask[4];
    GLboolean blend, depth_test, cull_face, scissor_test, stencil_test;
    GLboolean alpha_to_coverage, rasterizer_discard;
} gl_state;

static bool s_visible;          // atomic

// Frame timing, drawing thread only.
static u64 s_last_tick;
static float s_frames_ms[GRAPH_FRAMES];
static int s_frame_pos;
static float s_window_ms, s_window_worst, s_previous_worst;
static int s_window_frames;
static float s_shown_fps, s_shown_ms, s_shown_worst;

// GL objects, made the first time the overlay is shown.
static GLuint s_program, s_vertex_array, s_buffer;
static bool s_failed;
static vertex s_vertices[MAX_QUADS * 6];
static int s_vertex_count;
static float s_width, s_height;

void fpsoverlay_init(void) {
    FILE *f = fopen(DATA_PATH "config.ini", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int value = 0;
        if (sscanf(line, " fpsoverlay = %d", &value) == 1)
            __atomic_store_n(&s_visible, value != 0, __ATOMIC_RELAXED);
    }
    fclose(f);
}

void fpsoverlay_toggle(void) {
    bool visible = !__atomic_load_n(&s_visible, __ATOMIC_RELAXED);
    __atomic_store_n(&s_visible, visible, __ATOMIC_RELAXED);
    l_info("[overlay] frame rate overlay %s", visible ? "shown" : "hidden");
}

// ============================================================================
// Timing
// ============================================================================
static void time_frame(void) {
    u64 now = armGetSystemTick();
    u64 last = s_last_tick;
    s_last_tick = now;
    if (!last) return;

    float ms = (float)armTicksToNs(now - last) / 1000000.0f;
    // A gap this long is the console asleep or the game in the HOME menu,
    // not a frame.
    if (ms > 2000.0f) return;

    s_frames_ms[s_frame_pos] = ms;
    s_frame_pos = (s_frame_pos + 1) % GRAPH_FRAMES;

    s_window_ms += ms;
    s_window_frames++;
    if (ms > s_window_worst) s_window_worst = ms;
    if (s_window_ms >= WINDOW_MS) {
        s_shown_fps = (float)s_window_frames * 1000.0f / s_window_ms;
        s_shown_ms = s_window_ms / (float)s_window_frames;
        // The worst frame of the last two windows, so a hitch stays on
        // screen for about a second.
        s_shown_worst = s_window_worst > s_previous_worst ? s_window_worst : s_previous_worst;
        s_previous_worst = s_window_worst;
        s_window_ms = s_window_worst = 0.0f;
        s_window_frames = 0;
    }
}

// ============================================================================
// Rectangles and text. Coordinates are pixels from the top left corner.
// ============================================================================
typedef struct { uint8_t r, g, b, a; } color;

static const color WHITE  = { 255, 255, 255, 255 };
static const color GREY   = { 255, 255, 255, 90 };
static const color SHADE  = { 0, 0, 0, 205 };
static const color GREEN  = { 80, 220, 100, 255 };
static const color YELLOW = { 240, 200, 60, 255 };
static const color RED    = { 240, 70, 70, 255 };

static void rect(float x, float y, float w, float h, color c) {
    if (s_vertex_count + 6 > MAX_QUADS * 6) return;
    float x0 = x / s_width * 2.0f - 1.0f, x1 = (x + w) / s_width * 2.0f - 1.0f;
    float y0 = 1.0f - y / s_height * 2.0f, y1 = 1.0f - (y + h) / s_height * 2.0f;
    const float corners[6][2] = { { x0, y0 }, { x0, y1 }, { x1, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } };
    for (int i = 0; i < 6; i++) {
        vertex *v = &s_vertices[s_vertex_count++];
        v->x = corners[i][0];
        v->y = corners[i][1];
        v->r = c.r; v->g = c.g; v->b = c.b; v->a = c.a;
    }
}

// 5x7 glyphs, one byte per row, bit 4 the leftmost pixel.
static const struct { char ch; uint8_t rows[7]; } s_font[] = {
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
};

// Draws `str` with font pixels of `size` screen pixels; a glyph cell is six
// font pixels wide.
static void draw_text(float x, float y, float size, const char *str, color c) {
    for (; *str; str++, x += 6.0f * size) {
        const uint8_t *rows = NULL;
        for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
            if (s_font[i].ch == *str) rows = s_font[i].rows;
        }
        if (!rows) continue;
        for (int row = 0; row < 7; row++) {
            // One rectangle per run of lit pixels.
            for (int col = 0; col < 5; col++) {
                if (!(rows[row] & (0x10 >> col))) continue;
                int end = col;
                while (end + 1 < 5 && (rows[row] & (0x10 >> (end + 1)))) end++;
                rect(x + (float)col * size, y + (float)row * size, (float)(end - col + 1) * size, size, c);
                col = end;
            }
        }
    }
}

static void build_panel(void) {
    // Sized for 720 lines and scaled in whole steps from there.
    float u = (float)((int)s_height / 360);
    if (u < 1.0f) u = 1.0f;
    const float margin = 4.0f * u, pad = 3.0f * u;
    const float graph_w = (float)GRAPH_FRAMES * u, graph_h = 20.0f * u;
    const float big = 2.0f * u, small = u;

    float x = margin + pad, y = margin + pad;
    s_vertex_count = 0;
    rect(margin, margin, graph_w + 2.0f * pad, 7.0f * big + 7.0f * small + graph_h + 2.0f * pad + 5.0f * u, SHADE);

    char line[64];
    snprintf(line, sizeof(line), "%d FPS", (int)(s_shown_fps + 0.5f));
    draw_text(x, y, big, line, WHITE);
    y += 7.0f * big + 2.0f * u;

    int ms = (int)(s_shown_ms * 10.0f + 0.5f), worst = (int)(s_shown_worst * 10.0f + 0.5f);
    snprintf(line, sizeof(line), "%d.%d MS  MAX %d.%d", ms / 10, ms % 10, worst / 10, worst % 10);
    draw_text(x, y, small, line, WHITE);
    y += 7.0f * small + 3.0f * u;

    // Oldest frame on the left; the lines mark 60 and 30 FPS.
    for (int i = 0; i < GRAPH_FRAMES; i++) {
        float ms_i = s_frames_ms[(s_frame_pos + i) % GRAPH_FRAMES];
        float h = (ms_i > GRAPH_TOP_MS ? GRAPH_TOP_MS : ms_i) / GRAPH_TOP_MS * graph_h;
        rect(x + (float)i * u, y + graph_h - h, u, h, ms_i <= GOOD_MS ? GREEN : ms_i <= FAIR_MS ? YELLOW : RED);
    }
    rect(x, y + graph_h - graph_h * (1000.0f / 60.0f) / GRAPH_TOP_MS, graph_w, 1.0f, GREY);
    rect(x, y + graph_h - graph_h * (1000.0f / 30.0f) / GRAPH_TOP_MS, graph_w, 1.0f, GREY);
}

// ============================================================================
// GL
// ============================================================================
static GLuint compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Leaves its vertex array and buffer bound; the caller restores the bindings.
static bool create_objects(void) {
    static const char *const vertex_source =
        "attribute vec2 a_position;\n"
        "attribute vec4 a_color;\n"
        "varying vec4 v_color;\n"
        "void main() {\n"
        "    v_color = a_color;\n"
        "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
        "}\n";
    static const char *const fragment_source =
        "precision mediump float;\n"
        "varying vec4 v_color;\n"
        "void main() {\n"
        "    gl_FragColor = v_color;\n"
        "}\n";

    GLuint vs = compile(GL_VERTEX_SHADER, vertex_source);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_source);
    GLint ok = GL_FALSE;
    if (vs && fs) {
        s_program = glCreateProgram();
        glAttachShader(s_program, vs);
        glAttachShader(s_program, fs);
        glBindAttribLocation(s_program, 0, "a_position");
        glBindAttribLocation(s_program, 1, "a_color");
        glLinkProgram(s_program);
        glGetProgramiv(s_program, GL_LINK_STATUS, &ok);
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (!ok) {
        if (s_program) glDeleteProgram(s_program);
        s_program = 0;
        return false;
    }

    glGenVertexArrays(1, &s_vertex_array);
    glGenBuffers(1, &s_buffer);
    glBindVertexArray(s_vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, s_buffer);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (const void *)0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(vertex), (const void *)8);
    return true;
}

static void save_state(gl_state *s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->framebuffer);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s->blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blend_eq_alpha);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
    s->blend = glIsEnabled(GL_BLEND);
    s->depth_test = glIsEnabled(GL_DEPTH_TEST);
    s->cull_face = glIsEnabled(GL_CULL_FACE);
    s->scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    s->stencil_test = glIsEnabled(GL_STENCIL_TEST);
    s->alpha_to_coverage = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
    s->rasterizer_discard = glIsEnabled(GL_RASTERIZER_DISCARD);
}

static void set_enabled(GLenum cap, GLboolean on) {
    if (on) glEnable(cap); else glDisable(cap);
}

static void restore_state(const gl_state *s) {
    glUseProgram((GLuint)s->program);
    glBindVertexArray((GLuint)s->vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->array_buffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->framebuffer);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glBlendFuncSeparate((GLenum)s->blend_src_rgb, (GLenum)s->blend_dst_rgb,
                        (GLenum)s->blend_src_alpha, (GLenum)s->blend_dst_alpha);
    glBlendEquationSeparate((GLenum)s->blend_eq_rgb, (GLenum)s->blend_eq_alpha);
    glColorMask(s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3]);
    set_enabled(GL_BLEND, s->blend);
    set_enabled(GL_DEPTH_TEST, s->depth_test);
    set_enabled(GL_CULL_FACE, s->cull_face);
    set_enabled(GL_SCISSOR_TEST, s->scissor_test);
    set_enabled(GL_STENCIL_TEST, s->stencil_test);
    set_enabled(GL_SAMPLE_ALPHA_TO_COVERAGE, s->alpha_to_coverage);
    set_enabled(GL_RASTERIZER_DISCARD, s->rasterizer_discard);
}

static void draw(void) {
    // The window's size, not the surface's: Mesa's Switch EGL answers 0 x 0
    // to eglQuerySurface.
    u32 width = 0, height = 0;
    if (R_FAILED(nwindowGetDimensions(nwindowGetDefault(), &width, &height)) || !width || !height)
        return;
    s_width = (float)width;
    s_height = (float)height;
    build_panel();

    gl_state saved;
    save_state(&saved);

    // The objects go with the context: make them again if it was replaced.
    if (!s_program || !glIsProgram(s_program)) {
        if (!create_objects()) {
            s_failed = true;
            l_warn("[overlay] could not build the overlay shader; overlay disabled");
            restore_state(&saved);
            return;
        }
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_RASTERIZER_DISCARD);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    // The picture's alpha is left as the game wrote it.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(s_program);
    glBindVertexArray(s_vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, s_buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)s_vertex_count * sizeof(vertex)), s_vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, s_vertex_count);

    restore_state(&saved);
}

void fpsoverlay_frame(void) {
    time_frame();
    if (!s_failed && __atomic_load_n(&s_visible, __ATOMIC_RELAXED)) draw();
}
