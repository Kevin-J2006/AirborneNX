#ifndef __GLTRACE_H__
#define __GLTRACE_H__

#include <GLES3/gl31.h>

// Presented frame whose GL calls are recorded (0 = never).
#ifndef GLTRACE_FRAME
#define GLTRACE_FRAME 0
#endif

void gltrace_frame(unsigned long frame);

// Import-table entry for a GL function: the recording wrapper when a frame is
// being traced, the driver's own function otherwise.
#if GLTRACE_FRAME
#define GLTRACE_FN(fn) trace_##fn
#else
#define GLTRACE_FN(fn) fn
#endif

void trace_glBindTexture(GLenum target, GLuint texture);
void trace_glUseProgram(GLuint program);
void trace_glActiveTexture(GLenum texture);
void trace_glBindFramebuffer(GLenum target, GLuint framebuffer);
void trace_glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void trace_glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void trace_glEnable(GLenum cap);
void trace_glDisable(GLenum cap);
void trace_glBlendFunc(GLenum s, GLenum d);
void trace_glBlendFuncSeparate(GLenum sc, GLenum dc, GLenum sa, GLenum da);
void trace_glBlendEquation(GLenum mode);
void trace_glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void trace_glDepthFunc(GLenum func);
void trace_glDepthMask(GLboolean flag);
void trace_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void trace_glCullFace(GLenum mode);
void trace_glFrontFace(GLenum mode);
void trace_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void trace_glClear(GLbitfield mask);
void trace_glStencilFunc(GLenum func, GLint ref, GLuint mask);
void trace_glStencilOp(GLenum f, GLenum zf, GLenum zp);
void trace_glBindBuffer(GLenum target, GLuint buffer);
void trace_glEnableVertexAttribArray(GLuint index);
void trace_glDisableVertexAttribArray(GLuint index);
void trace_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
void trace_glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void trace_glVertexAttrib4fv(GLuint index, const GLfloat *v);
void trace_glUniform1i(GLint loc, GLint v);
void trace_glUniform1iv(GLint loc, GLsizei count, const GLint *v);
void trace_glUniform1f(GLint loc, GLfloat v);
void trace_glUniform2f(GLint loc, GLfloat x, GLfloat y);
void trace_glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z);
void trace_glUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void trace_glUniform1fv(GLint loc, GLsizei count, const GLfloat *v);
void trace_glUniform2fv(GLint loc, GLsizei count, const GLfloat *v);
void trace_glUniform3fv(GLint loc, GLsizei count, const GLfloat *v);
void trace_glUniform4fv(GLint loc, GLsizei count, const GLfloat *v);
void trace_glUniformMatrix3fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v);
void trace_glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v);
void trace_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void trace_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);

#endif // __GLTRACE_H__
