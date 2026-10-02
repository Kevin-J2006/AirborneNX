#ifndef __REIMPL_EGL_H__
#define __REIMPL_EGL_H__

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdbool.h>

/*
 * Asphalt 8 v4.x owns its EGL display, contexts and render thread; these
 * wrappers redirect it from the Android ANativeWindow to the Switch NWindow
 * and back all of its contexts with a single real one.
 */
EGLDisplay wrap_eglGetDisplay(EGLNativeDisplayType display_id);
EGLBoolean wrap_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                                EGLint config_size, EGLint *num_config);
EGLSurface wrap_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, void *win,
                                       const EGLint *attrib_list);
EGLSurface wrap_eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list);
EGLBoolean wrap_eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean wrap_eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value);
EGLContext wrap_eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                                 const EGLint *attrib_list);
EGLBoolean wrap_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value);
EGLBoolean wrap_eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean wrap_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLBoolean wrap_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);
EGLBoolean wrap_eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLContext wrap_eglGetCurrentContext(void);

// Gives the calling thread the single real context with its own bindings.
// Called with the GL lock held; see egl.c.
void egl_ctx_acquire(void);

// Whether the calling thread is the one that presents frames (it owns the
// window surface). Called with the GL lock held.
bool egl_on_drawing_thread(void);

// Number of frames the game has presented so far.
unsigned long egl_frame_count(void);

#endif // __REIMPL_EGL_H__
