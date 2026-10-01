#ifndef __GLDIAG_H__
#define __GLDIAG_H__

#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <stddef.h>
#include "../utils/logger.h"
#include "../utils/prof.h"
#include "astc.h"

// glGetString with the renderer name the engine's quality profiles expect.
const GLubyte *wrap_glGetString(GLenum name);

// Import-table entry for a GL function: the checking wrapper in debug builds,
// the driver's own function otherwise.
#if AIRBORNE_DEBUG
#define GLDIAG_FN(fn) diag_##fn
#elif AIRBORNE_PROFILE
#define GLDIAG_FN(fn) GLPROF_##fn
#else
#define GLDIAG_FN(fn) fn
#endif

// glCompressedTexImage2D always ends in astc_glCompressedTexImage2D.
#if AIRBORNE_DEBUG || AIRBORNE_PROFILE
#define GL_COMPRESSED_TEX_IMPORT GLDIAG_FN(glCompressedTexImage2D)
#else
#define GL_COMPRESSED_TEX_IMPORT astc_glCompressedTexImage2D
#endif

// Profile build: time the calls that can stall a frame, pass the rest through.
#define GLPROF_glCompileShader          prof_glCompileShader
#define GLPROF_glLinkProgram            prof_glLinkProgram
#define GLPROF_glTexImage2D             prof_glTexImage2D
#define GLPROF_glCompressedTexImage2D   prof_glCompressedTexImage2D
#define GLPROF_glTexSubImage2D          glTexSubImage2D
#define GLPROF_glRenderbufferStorage    glRenderbufferStorage
#define GLPROF_glCheckFramebufferStatus glCheckFramebufferStatus
#define GLPROF_glGenerateMipmap         glGenerateMipmap
#define GLPROF_glTexParameteri          glTexParameteri
#define GLPROF_glTexParameterf          glTexParameterf
#define GLPROF_glFramebufferTexture2D   glFramebufferTexture2D
void prof_glCompileShader(GLuint shader);
void prof_glLinkProgram(GLuint program);
void prof_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                       GLint border, GLenum format, GLenum type, const void *pixels);
void prof_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border, GLsizei imageSize, const void *data);

// Pass-through GL entry points that log what the engine asks the driver for
// and every call the driver rejects.
void diag_glCompileShader(GLuint shader);
void diag_glLinkProgram(GLuint program);
void diag_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border, GLsizei imageSize, const void *data);
void diag_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                       GLint border, GLenum format, GLenum type, const void *pixels);
void diag_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                          GLsizei height, GLenum format, GLenum type, const void *pixels);
void diag_glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
GLenum diag_glCheckFramebufferStatus(GLenum target);
void diag_glGenerateMipmap(GLenum target);
void diag_glTexParameteri(GLenum target, GLenum pname, GLint param);
void diag_glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void diag_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void gldiag_frame(unsigned long frame);
void gldiag_describe_texture(GLuint name, char *out, size_t size);

#endif // __GLDIAG_H__
