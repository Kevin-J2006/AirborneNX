#ifndef __REIMPL_ASTC_H__
#define __REIMPL_ASTC_H__

#include <GLES2/gl2.h>

// glCompressedTexImage2D that decompresses ASTC without holding the GL lock.
void astc_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border, GLsizei imageSize, const void *data);

#endif // __REIMPL_ASTC_H__
