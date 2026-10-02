#ifndef __REIMPL_SHADERCACHE_H__
#define __REIMPL_SHADERCACHE_H__

#include <GLES3/gl31.h>

// Linked shader programs kept on the SD card between runs (see shadercache.c).
// These take the place of the GL entry points of the same name.
GLuint sc_glCreateShader(GLenum type);
GLuint sc_glCreateProgram(void);
void sc_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
void sc_glCompileShader(GLuint shader);
void sc_glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
void sc_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
void sc_glAttachShader(GLuint program, GLuint shader);
void sc_glDetachShader(GLuint program, GLuint shader);
void sc_glBindAttribLocation(GLuint program, GLuint index, const GLchar *name);
void sc_glLinkProgram(GLuint program);

// Programs taken from the cache / built from source since the last call.
void shadercache_stats(unsigned *loaded, unsigned *built);

#endif // __REIMPL_SHADERCACHE_H__
