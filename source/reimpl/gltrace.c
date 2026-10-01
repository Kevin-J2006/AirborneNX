#include "gltrace.h"
#include "gldiag.h"
#include "../utils/init.h"
#include "../utils/logger.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One-frame GL call recorder. gltrace_frame() arms it for the frame given in
// GLTRACE_FRAME; every wrapped call made during that frame is written to
// GLTRACE_PATH together with the sources of the programs it used.
#define GLTRACE_PATH DATA_PATH "gltrace.txt"

static FILE *s_out;
static unsigned long s_target_frame = GLTRACE_FRAME;
static unsigned char s_program_dumped[4096];

#define T(...) do { if (s_out) fprintf(s_out, __VA_ARGS__); } while (0)

static void dump_program(GLuint program) {
    if (program == 0 || program >= sizeof(s_program_dumped) || s_program_dumped[program]) return;
    s_program_dumped[program] = 1;

    GLuint shaders[4];
    GLsizei count = 0;
    glGetAttachedShaders(program, 4, &count, shaders);
    T("=== program %u (%d shaders)\n", program, count);
    for (GLsizei i = 0; i < count; i++) {
        GLint type = 0, len = 0;
        glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
        glGetShaderiv(shaders[i], GL_SHADER_SOURCE_LENGTH, &len);
        char *src = (char *)malloc((size_t)len + 1);
        if (!src) continue;
        src[0] = 0;
        glGetShaderSource(shaders[i], len + 1, NULL, src);
        T("--- %s shader %u\n%s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", shaders[i], src);
        free(src);
    }

    GLint n = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &n);
    for (GLint i = 0; i < n; i++) {
        char name[128];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, (GLuint)i, sizeof(name), NULL, &size, &type, name);
        T("--- uniform loc %d: %s type 0x%x size %d\n", glGetUniformLocation(program, name), name, type, size);
    }
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &n);
    for (GLint i = 0; i < n; i++) {
        char name[128];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveAttrib(program, (GLuint)i, sizeof(name), NULL, &size, &type, name);
        T("--- attrib loc %d: %s type 0x%x\n", glGetAttribLocation(program, name), name, type);
    }
    T("=== end program %u\n", program);
}

void gltrace_frame(unsigned long frame) {
    if (s_out) {
        fclose(s_out);
        s_out = NULL;
        l_info("[GL] frame %lu traced to %s", frame, GLTRACE_PATH);
    }
    if (frame + 1 == s_target_frame) {
        s_out = fopen(GLTRACE_PATH, "wb");
        if (s_out) setvbuf(s_out, NULL, _IOFBF, 1 << 16);
        memset(s_program_dumped, 0, sizeof(s_program_dumped));
        T("# frame %lu\n", frame + 1);
    }
}

static void texture_line(GLenum target, GLuint texture) {
    if (!s_out) return;
    GLint w = 0, h = 0, fmt = 0, minf = 0, magf = 0, ws = 0, wt = 0;
    // GLES 3.1 level queries; the context Mesa hands out supports them.
    if (target == GL_TEXTURE_2D && texture) {
        glGetTexLevelParameteriv(target, 0, 0x1000, &w);   // GL_TEXTURE_WIDTH
        glGetTexLevelParameteriv(target, 0, 0x1001, &h);   // GL_TEXTURE_HEIGHT
        glGetTexLevelParameteriv(target, 0, 0x1003, &fmt); // GL_TEXTURE_INTERNAL_FORMAT
    }
    if (texture) {
        glGetTexParameteriv(target, GL_TEXTURE_MIN_FILTER, &minf);
        glGetTexParameteriv(target, GL_TEXTURE_MAG_FILTER, &magf);
        glGetTexParameteriv(target, GL_TEXTURE_WRAP_S, &ws);
        glGetTexParameteriv(target, GL_TEXTURE_WRAP_T, &wt);
    }
    char desc[160];
    gldiag_describe_texture(texture, desc, sizeof(desc));
    T("glBindTexture(0x%x, %u) [%dx%d fmt 0x%x min 0x%x mag 0x%x wrap 0x%x/0x%x %s]\n",
      target, texture, w, h, fmt, minf, magf, ws, wt, desc);
    while (glGetError() != GL_NO_ERROR) {}
}

void trace_glBindTexture(GLenum target, GLuint texture) {
    glBindTexture(target, texture);
    texture_line(target, texture);
}

void trace_glUseProgram(GLuint program) {
    glUseProgram(program);
    if (!s_out) return;
    dump_program(program);
    T("glUseProgram(%u)\n", program);
}

void trace_glActiveTexture(GLenum texture) {
    glActiveTexture(texture);
    T("glActiveTexture(%d)\n", (int)(texture - GL_TEXTURE0));
}

void trace_glBindFramebuffer(GLenum target, GLuint framebuffer) {
    glBindFramebuffer(target, framebuffer);
    if (!s_out) return;
    GLint type = 0, name = 0;
    if (framebuffer) {
        glGetFramebufferAttachmentParameteriv(target, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        glGetFramebufferAttachmentParameteriv(target, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    }
    T("glBindFramebuffer(%u) [color type 0x%x name %d]\n", framebuffer, type, name);
}

void trace_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    glViewport(x, y, w, h);
    T("glViewport(%d, %d, %d, %d)\n", x, y, w, h);
}

void trace_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    glScissor(x, y, w, h);
    T("glScissor(%d, %d, %d, %d)\n", x, y, w, h);
}

void trace_glEnable(GLenum cap) { glEnable(cap); T("glEnable(0x%x)\n", cap); }
void trace_glDisable(GLenum cap) { glDisable(cap); T("glDisable(0x%x)\n", cap); }

void trace_glBlendFunc(GLenum s, GLenum d) { glBlendFunc(s, d); T("glBlendFunc(0x%x, 0x%x)\n", s, d); }
void trace_glBlendFuncSeparate(GLenum sc, GLenum dc, GLenum sa, GLenum da) {
    glBlendFuncSeparate(sc, dc, sa, da);
    T("glBlendFuncSeparate(0x%x, 0x%x, 0x%x, 0x%x)\n", sc, dc, sa, da);
}
void trace_glBlendEquation(GLenum mode) { glBlendEquation(mode); T("glBlendEquation(0x%x)\n", mode); }
void trace_glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    glBlendColor(r, g, b, a);
    T("glBlendColor(%g, %g, %g, %g)\n", r, g, b, a);
}
void trace_glDepthFunc(GLenum func) { glDepthFunc(func); T("glDepthFunc(0x%x)\n", func); }
void trace_glDepthMask(GLboolean flag) { glDepthMask(flag); T("glDepthMask(%d)\n", flag); }
void trace_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    glColorMask(r, g, b, a);
    T("glColorMask(%d, %d, %d, %d)\n", r, g, b, a);
}
void trace_glCullFace(GLenum mode) { glCullFace(mode); T("glCullFace(0x%x)\n", mode); }
void trace_glFrontFace(GLenum mode) { glFrontFace(mode); T("glFrontFace(0x%x)\n", mode); }
void trace_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    glClearColor(r, g, b, a);
    T("glClearColor(%g, %g, %g, %g)\n", r, g, b, a);
}
void trace_glClear(GLbitfield mask) { glClear(mask); T("glClear(0x%x)\n", mask); }
void trace_glStencilFunc(GLenum func, GLint ref, GLuint mask) {
    glStencilFunc(func, ref, mask);
    T("glStencilFunc(0x%x, %d, 0x%x)\n", func, ref, mask);
}
void trace_glStencilOp(GLenum f, GLenum zf, GLenum zp) { glStencilOp(f, zf, zp); T("glStencilOp(0x%x, 0x%x, 0x%x)\n", f, zf, zp); }

void trace_glBindBuffer(GLenum target, GLuint buffer) { glBindBuffer(target, buffer); T("glBindBuffer(0x%x, %u)\n", target, buffer); }
void trace_glEnableVertexAttribArray(GLuint index) { glEnableVertexAttribArray(index); T("glEnableVertexAttribArray(%u)\n", index); }
void trace_glDisableVertexAttribArray(GLuint index) { glDisableVertexAttribArray(index); T("glDisableVertexAttribArray(%u)\n", index); }
void trace_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
    T("glVertexAttribPointer(%u, size %d, type 0x%x, norm %d, stride %d, ptr %p)\n", index, size, type, normalized, stride, pointer);
}
void trace_glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    glVertexAttrib4f(index, x, y, z, w);
    T("glVertexAttrib4f(%u, %g, %g, %g, %g)\n", index, x, y, z, w);
}
void trace_glVertexAttrib4fv(GLuint index, const GLfloat *v) {
    glVertexAttrib4fv(index, v);
    T("glVertexAttrib4fv(%u, %g, %g, %g, %g)\n", index, v[0], v[1], v[2], v[3]);
}

static void floats(const char *name, GLint loc, GLsizei count, int per, const GLfloat *v) {
    if (!s_out) return;
    T("%s(loc %d, count %d:", name, loc, count);
    int n = count * per;
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) T(" %g", v[i]);
    T(")\n");
}

void trace_glUniform1i(GLint loc, GLint v) { glUniform1i(loc, v); T("glUniform1i(loc %d, %d)\n", loc, v); }
void trace_glUniform1iv(GLint loc, GLsizei count, const GLint *v) {
    glUniform1iv(loc, count, v);
    T("glUniform1iv(loc %d, count %d, %d...)\n", loc, count, count > 0 ? v[0] : 0);
}
void trace_glUniform1f(GLint loc, GLfloat v) { glUniform1f(loc, v); T("glUniform1f(loc %d, %g)\n", loc, v); }
void trace_glUniform2f(GLint loc, GLfloat x, GLfloat y) { glUniform2f(loc, x, y); T("glUniform2f(loc %d, %g, %g)\n", loc, x, y); }
void trace_glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z) { glUniform3f(loc, x, y, z); T("glUniform3f(loc %d, %g, %g, %g)\n", loc, x, y, z); }
void trace_glUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    glUniform4f(loc, x, y, z, w);
    T("glUniform4f(loc %d, %g, %g, %g, %g)\n", loc, x, y, z, w);
}
void trace_glUniform1fv(GLint loc, GLsizei count, const GLfloat *v) { glUniform1fv(loc, count, v); floats("glUniform1fv", loc, count, 1, v); }
void trace_glUniform2fv(GLint loc, GLsizei count, const GLfloat *v) { glUniform2fv(loc, count, v); floats("glUniform2fv", loc, count, 2, v); }
void trace_glUniform3fv(GLint loc, GLsizei count, const GLfloat *v) { glUniform3fv(loc, count, v); floats("glUniform3fv", loc, count, 3, v); }
void trace_glUniform4fv(GLint loc, GLsizei count, const GLfloat *v) { glUniform4fv(loc, count, v); floats("glUniform4fv", loc, count, 4, v); }
void trace_glUniformMatrix3fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v) {
    glUniformMatrix3fv(loc, count, transpose, v);
    floats("glUniformMatrix3fv", loc, count, 9, v);
}
void trace_glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v) {
    glUniformMatrix4fv(loc, count, transpose, v);
    floats("glUniformMatrix4fv", loc, count, 16, v);
}

static void after_draw(void) {
    if (!s_out) return;
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) T("  !! GL error 0x%x\n", err);
}

void trace_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    glDrawArrays(mode, first, count);
    T("glDrawArrays(mode 0x%x, first %d, count %d)\n", mode, first, count);
    after_draw();
}

void trace_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    glDrawElements(mode, count, type, indices);
    T("glDrawElements(mode 0x%x, count %d, type 0x%x, indices %p)\n", mode, count, type, indices);
    after_draw();
}
