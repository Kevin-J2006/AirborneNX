#include "astc.h"
#include "../utils/prof.h"
#include <stdint.h>
#include <stdlib.h>

// Mesa internals (libEGL.a carries all of Mesa). mesa_format is an enum.
extern unsigned _mesa_glenum_to_compressed_format(GLenum format);
extern void _mesa_unpack_astc_2d_ldr(uint8_t *dst_row, int dst_stride, const uint8_t *src_row, int src_stride,
                                     unsigned src_width, unsigned src_height, unsigned format);

extern void gl_lock_pause(void);
extern void gl_lock_resume(void);

// GL_COMPRESSED_RGBA_ASTC_4x4_KHR .. 12x12_KHR, in enum order.
static const struct { uint8_t w, h; } s_blocks[14] = {
    { 4, 4 }, { 5, 4 }, { 5, 5 }, { 6, 5 }, { 6, 6 }, { 8, 5 }, { 8, 6 },
    { 8, 8 }, { 10, 5 }, { 10, 6 }, { 10, 8 }, { 10, 10 }, { 12, 10 }, { 12, 12 },
};

// The Switch GPU driver has no ASTC support: Mesa accepts the format and
// decompresses every upload to RGBA8 on the CPU, inside the GL call. For a
// 1024x1024 texture that is about half a second during which the caller
// holds the GL lock, so a texture streamed in by the loader thread froze the
// thread that draws for as long. Run the same decoder here with the lock
// released and hand the driver plain RGBA8, which it only has to copy.
void astc_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border, GLsizei imageSize, const void *data) {
    unsigned index = internalformat - 0x93B0; // GL_COMPRESSED_RGBA_ASTC_4x4_KHR
    uint8_t *pixels = NULL;
    if (index < 14 && data && width > 0 && height > 0 && border == 0) {
        unsigned bw = s_blocks[index].w, bh = s_blocks[index].h;
        size_t blocks_x = (width + bw - 1) / bw, blocks_y = (height + bh - 1) / bh;
        if ((size_t)imageSize == blocks_x * blocks_y * 16)
            pixels = (uint8_t *)malloc((size_t)width * height * 4);

        if (pixels) {
            gl_lock_pause();
            PROF_BEGIN();
            _mesa_unpack_astc_2d_ldr(pixels, width * 4, (const uint8_t *)data, (int)(blocks_x * 16),
                                     (unsigned)width, (unsigned)height,
                                     _mesa_glenum_to_compressed_format(internalformat));
            PROF_END(PROF_DECODE);
            gl_lock_resume();
        }
    }

    if (!pixels) {
        glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
        return;
    }

    // Rows are width * 4 bytes, so any unpack alignment the engine set fits.
    glTexImage2D(target, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
}
