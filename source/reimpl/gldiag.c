#include "gldiag.h"
#include "../utils/logger.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

// Reports each distinct (kind, a, b, c) combination once, plus every GL error.
#define SEEN_MAX 256
static struct { int kind; GLenum a, b, c; } s_seen[SEEN_MAX];
static int s_seen_count;
static int s_error_budget = 200;

static bool first_time(int kind, GLenum a, GLenum b, GLenum c) {
    for (int i = 0; i < s_seen_count; i++)
        if (s_seen[i].kind == kind && s_seen[i].a == a && s_seen[i].b == b && s_seen[i].c == c)
            return false;
    if (s_seen_count >= SEEN_MAX) return false;
    s_seen[s_seen_count].kind = kind;
    s_seen[s_seen_count].a = a;
    s_seen[s_seen_count].b = b;
    s_seen[s_seen_count].c = c;
    s_seen_count++;
    return true;
}

// What the engine did to each texture name, to spot incomplete textures.
#define TEX_MAX 16384
typedef struct {
    GLenum format;      // internal format of level 0
    uint16_t w, h;
    uint8_t levels;     // highest level uploaded + 1
    uint8_t compressed, has_data, mip_filter, max_level_set, genmip, cube;
    int tid;
} tex_info;
static tex_info s_tex[TEX_MAX];

extern int pthr_gettid(void);

static tex_info *bound_tex(GLenum target) {
    GLint name = 0;
    glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_CUBE_MAP, &name);
    if (name <= 0 || name >= TEX_MAX) return NULL;
    s_tex[name].cube = (target != GL_TEXTURE_2D);
    return &s_tex[name];
}

static void note_upload(GLenum target, GLint level, GLenum format, GLsizei w, GLsizei h, bool compressed, bool data) {
    tex_info *t = bound_tex(target);
    if (!t) return;
    if (level == 0) { t->format = format; t->w = (uint16_t)w; t->h = (uint16_t)h; t->compressed = compressed; }
    if (level + 1 > t->levels) t->levels = (uint8_t)(level + 1);
    if (data) t->has_data = 1;
    t->tid = pthr_gettid();
}

static void drain_errors(void) {
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++) {}
}

static bool take_error(GLenum *err) {
    *err = glGetError();
    if (*err == GL_NO_ERROR || s_error_budget <= 0) return false;
    s_error_budget--;
    return true;
}

// GameOptions.json selects the GPU profile by substring of GL_RENDERER. Mesa's
// "NV120" matches nothing, which leaves the VERYLOW / 30 FPS defaults; any
// name containing "GeForce" selects GPU_5, the tier of the Tegra X1's peers.
#define GUEST_GL_RENDERER "NVIDIA GeForce GM20B"

const GLubyte *wrap_glGetString(GLenum name) {
    const GLubyte *s = glGetString(name);
    if (name == GL_RENDERER && s) {
        if (first_time(0, name, 1, 0))
            l_info("[GL] GL_RENDERER %s reported to the game as %s", (const char *)s, GUEST_GL_RENDERER);
        return (const GLubyte *)GUEST_GL_RENDERER;
    }
    if (first_time(0, name, 0, 0)) {
        if (name == GL_EXTENSIONS && s) {
            // The logger truncates long lines, so split the list.
            const char *p = (const char *)s;
            size_t len = strlen(p);
            for (size_t off = 0; off < len; off += 400)
                l_info("[GL] GL_EXTENSIONS[%zu]: %.400s", off, p + off);
        } else {
            l_info("[GL] glGetString(0x%x) -> %s", name, s ? (const char *)s : "(null)");
        }
    }
    return s;
}

void diag_glCompileShader(GLuint shader) {
    sc_glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return;

    static int s_budget = 12;
    if (s_budget <= 0) return;
    s_budget--;

    char info[1024] = "";
    glGetShaderInfoLog(shader, sizeof(info), NULL, info);
    l_error("[GL] shader %u failed to compile: %s", shader, info);

    GLint len = 0;
    glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &len);
    char *src = (len > 0) ? (char *)malloc((size_t)len + 1) : NULL;
    if (!src) return;
    glGetShaderSource(shader, len, NULL, src);
    src[len] = 0;
    for (GLint off = 0; off < len && off < 6000; off += 400)
        l_error("[GL] shader %u source[%d]: %.400s", shader, off, src + off);
    free(src);
}

void diag_glLinkProgram(GLuint program) {
    sc_glLinkProgram(program);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok) return;

    static int s_budget = 30;
    if (s_budget <= 0) return;
    s_budget--;
    char info[1024] = "";
    glGetProgramInfoLog(program, sizeof(info), NULL, info);
    l_error("[GL] program %u failed to link: %s", program, info);
}

void diag_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border, GLsizei imageSize, const void *data) {
    drain_errors();
    astc_glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    note_upload(target, level, internalformat, width, height, true, data != NULL);
    GLenum err;
    bool failed = take_error(&err);
    if (failed || first_time(1, internalformat, 0, 0))
        l_info("[GL] glCompressedTexImage2D(target 0x%x, level %d, format 0x%x, %dx%d, %d bytes) -> 0x%x",
               target, level, internalformat, width, height, imageSize, err);
}

void diag_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                       GLint border, GLenum format, GLenum type, const void *pixels) {
    drain_errors();
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    note_upload(target, level, (GLenum)internalformat, width, height, false, pixels != NULL);
    GLenum err;
    bool failed = take_error(&err);
    if (failed || first_time(2, (GLenum)internalformat, format, type))
        l_info("[GL] glTexImage2D(target 0x%x, level %d, internal 0x%x, %dx%d, format 0x%x, type 0x%x, data %p) -> 0x%x",
               target, level, internalformat, width, height, format, type, pixels, err);
}

void diag_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                          GLsizei height, GLenum format, GLenum type, const void *pixels) {
    drain_errors();
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    GLenum err;
    bool failed = take_error(&err);
    if (failed || first_time(3, format, type, 0))
        l_info("[GL] glTexSubImage2D(target 0x%x, level %d, %d,%d %dx%d, format 0x%x, type 0x%x) -> 0x%x",
               target, level, xoffset, yoffset, width, height, format, type, err);
}

void diag_glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    drain_errors();
    glRenderbufferStorage(target, internalformat, width, height);
    GLenum err;
    bool failed = take_error(&err);
    if (failed || first_time(4, internalformat, 0, 0))
        l_info("[GL] glRenderbufferStorage(format 0x%x, %dx%d) -> 0x%x", internalformat, width, height, err);
}

GLenum diag_glCheckFramebufferStatus(GLenum target) {
    GLenum status = glCheckFramebufferStatus(target);
    if (status != GL_FRAMEBUFFER_COMPLETE && s_error_budget > 0) {
        s_error_budget--;
        GLint type = 0, name = 0, dtype = 0, dname = 0;
        glGetFramebufferAttachmentParameteriv(target, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        glGetFramebufferAttachmentParameteriv(target, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
        glGetFramebufferAttachmentParameteriv(target, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &dtype);
        glGetFramebufferAttachmentParameteriv(target, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &dname);
        l_error("[GL] framebuffer incomplete: 0x%x (color type 0x%x name %d, depth type 0x%x name %d)",
                status, type, name, dtype, dname);
    }
    return status;
}

void diag_glGenerateMipmap(GLenum target) {
    drain_errors();
    glGenerateMipmap(target);
    { tex_info *t = bound_tex(target); if (t) t->genmip = 1; }
    GLenum err;
    if (take_error(&err)) l_info("[GL] glGenerateMipmap(0x%x) -> 0x%x", target, err);
}

void diag_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    drain_errors();
    glTexParameteri(target, pname, param);
    GLenum err;
    bool failed = take_error(&err);
    if (failed || first_time(5, pname, (GLenum)param, 0))
        l_info("[GL] glTexParameteri(0x%x, 0x%x, 0x%x) -> 0x%x", target, pname, param, err);
    tex_info *t = (target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP) ? bound_tex(target) : NULL;
    if (!t) return;
    if (pname == GL_TEXTURE_MIN_FILTER) t->mip_filter = (param != GL_NEAREST && param != GL_LINEAR);
    if (pname == 0x813D) t->max_level_set = 1; // GL_TEXTURE_MAX_LEVEL
}

void diag_glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    drain_errors();
    glTexParameterf(target, pname, param);
    GLenum err;
    bool failed = take_error(&err);
    if (failed || first_time(6, pname, (GLenum)(GLint)param, 0))
        l_info("[GL] glTexParameterf(0x%x, 0x%x, %f) -> 0x%x", target, pname, param, err);
}

void diag_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    drain_errors();
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
    GLenum err;
    bool failed = take_error(&err);
    GLenum fmt = (texture < TEX_MAX) ? s_tex[texture].format : 0;
    if (failed || first_time(7, attachment, fmt, textarget))
        l_info("[GL] glFramebufferTexture2D(attachment 0x%x, textarget 0x%x, tex %u [format 0x%x %dx%d], level %d) -> 0x%x",
               attachment, textarget, texture, fmt, texture < TEX_MAX ? s_tex[texture].w : 0,
               texture < TEX_MAX ? s_tex[texture].h : 0, level, err);
}

static int full_chain(int w, int h) {
    int n = 1;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
    return n;
}

// Called once per presented frame, with the context current.
void gldiag_frame(unsigned long frame) {
    static int s_frame_errors = 40;
    GLenum err;
    for (int i = 0; i < 8 && (err = glGetError()) != GL_NO_ERROR; i++) {
        if (s_frame_errors <= 0) break;
        s_frame_errors--;
        l_error("[GL] error 0x%x pending at the end of frame %lu", err, frame);
    }
}

// One-line description of a texture for the frame trace, with the colour of
// its first texel when it is small enough to be a placeholder.
void gldiag_describe_texture(GLuint name, char *out, size_t size) {
    out[0] = 0;
    if (name == 0 || name >= TEX_MAX) return;
    tex_info *t = &s_tex[name];
    int n = snprintf(out, size, "levels %d/%d%s%s%s tid %d", t->levels, full_chain(t->w, t->h),
                     t->has_data ? "" : " no-data", t->genmip ? " genmip" : "",
                     t->max_level_set ? " max-level" : "", t->tid);
    if (t->cube || t->compressed || t->w > 16 || t->h > 16 || n < 0 || (size_t)n >= size) return;

    GLint prev = 0;
    GLuint fbo = 0;
    unsigned char px[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, name, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE) glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
    glDeleteFramebuffers(1, &fbo);
    snprintf(out + n, size - (size_t)n, " texel0 %02x%02x%02x%02x (fbo 0x%x)", px[0], px[1], px[2], px[3], status);
}

#if AIRBORNE_PROFILE
void prof_glCompileShader(GLuint shader) {
    PROF_BEGIN();
    sc_glCompileShader(shader);
    PROF_END(PROF_COMPILE);
}

void prof_glLinkProgram(GLuint program) {
    PROF_BEGIN();
    sc_glLinkProgram(program);
    PROF_END(PROF_LINK);
}

void prof_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                       GLint border, GLenum format, GLenum type, const void *pixels) {
    PROF_BEGIN();
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    PROF_END(PROF_TEXTURE);
}

void prof_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border, GLsizei imageSize, const void *data) {
    PROF_BEGIN();
    astc_glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    PROF_END(PROF_TEXTURE);
}
#endif // AIRBORNE_PROFILE
