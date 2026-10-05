#include "egl.h"
#include "../utils/init.h"
#include "../utils/logger.h"
#include "pthr.h"
#include "gldiag.h"
#include "gltrace.h"
#include "fpsoverlay.h"
#include "../utils/prof.h"
#include <switch.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// Token returned in place of a real pbuffer surface.
#define FAKE_PBUFFER ((EGLSurface)(uintptr_t)0x50425546)

static unsigned long s_frames = 0;
static EGLint s_requested_rgba[4] = { -1, -1, -1, -1 };

unsigned long egl_frame_count(void) {
    return __atomic_load_n(&s_frames, __ATOMIC_RELAXED);
}

EGLDisplay wrap_eglGetDisplay(EGLNativeDisplayType display_id) {
    (void)display_id;
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    l_info("[EGL] eglGetDisplay -> %p", dpy);
    return dpy;
}

EGLBoolean wrap_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                                EGLint config_size, EGLint *num_config) {
    for (const EGLint *a = attrib_list; a && *a != EGL_NONE; a += 2) {
        l_debug("[EGL] eglChooseConfig attrib 0x%04x = 0x%x", a[0], a[1]);
        switch (a[0]) {
            case EGL_RED_SIZE:   s_requested_rgba[0] = a[1]; break;
            case EGL_GREEN_SIZE: s_requested_rgba[1] = a[1]; break;
            case EGL_BLUE_SIZE:  s_requested_rgba[2] = a[1]; break;
            case EGL_ALPHA_SIZE: s_requested_rgba[3] = a[1]; break;
        }
    }

    // Mesa on Switch has no pbuffer configs. glf uses 1x1 pbuffers only to
    // make its loader-thread contexts current, so hand it window configs and
    // back the "pbuffer" with a surfaceless context (see FAKE_PBUFFER).
    EGLint patched[64];
    int n = 0;
    for (const EGLint *a = attrib_list; a && *a != EGL_NONE && n < 60; a += 2) {
        patched[n++] = a[0];
        patched[n++] = (a[0] == EGL_SURFACE_TYPE && (a[1] & EGL_PBUFFER_BIT))
                           ? ((a[1] & ~EGL_PBUFFER_BIT) | EGL_WINDOW_BIT) : a[1];
    }
    patched[n] = EGL_NONE;
    if (attrib_list) attrib_list = patched;

    EGLint count = 0;
    EGLBoolean ok = eglChooseConfig(dpy, attrib_list, configs, config_size, &count);
    if ((!ok || count == 0) && configs && config_size > 0) {
        static const EGLint fallback[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
            EGL_NONE
        };
        l_warn("[EGL] eglChooseConfig found nothing (err 0x%x); using fallback config", eglGetError());
        ok = eglChooseConfig(dpy, fallback, configs, config_size, &count);
    }
    if (num_config) *num_config = count;
    l_info("[EGL] eglChooseConfig -> ok=%d, %d config(s)", ok, count);
    return ok;
}

EGLSurface wrap_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, void *win,
                                       const EGLint *attrib_list) {
    (void)win; (void)attrib_list;
    NWindow *nwin = nwindowGetDefault();
    nwindowSetDimensions(nwin, ASPHALT8_RENDER_WIDTH_DEFAULT, ASPHALT8_RENDER_HEIGHT_DEFAULT);

    EGLSurface surface = eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)nwin, NULL);
    if (surface == EGL_NO_SURFACE)
        l_error("[EGL] eglCreateWindowSurface failed: 0x%x", eglGetError());
    else
        l_info("[EGL] eglCreateWindowSurface -> %p", surface);
    return surface;
}

EGLSurface wrap_eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) {
    (void)dpy; (void)config; (void)attrib_list;
    l_info("[EGL] eglCreatePbufferSurface -> surfaceless stand-in");
    return FAKE_PBUFFER;
}

EGLBoolean wrap_eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    if (surface == FAKE_PBUFFER) return EGL_TRUE;
    return eglDestroySurface(dpy, surface);
}

EGLBoolean wrap_eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
    if (surface == FAKE_PBUFFER) {
        if (value) *value = 1;
        return EGL_TRUE;
    }
    return eglQuerySurface(dpy, surface, attribute, value);
}

// ============================================================================
// One real context for every guest context.
//
// The engine uploads textures and buffers from a loader thread through a
// second context created with share_context. Mesa's Switch EGL driver accepts
// that argument and ignores it: the two contexts get separate object
// namespaces, so nothing the loader creates exists in the context that draws.
//
// Every guest context is therefore an alias of the first one. EGL only lets a
// context be current on one thread, and moving it with eglMakeCurrent for
// every loader call costs a flush each way, so the other threads are attached
// below EGL: glapi keeps the current context and dispatch table in two
// thread-local pointers, and they are simply set to the real context's. That
// is safe because every GL import already runs under the global GL lock (see
// gl_lock_enter in dynlib.c), which is also where the binding state the
// engine caches per context is swapped when a different thread comes in.
// ============================================================================
#define CTX_TEXTURE_UNITS 8

extern void *_glapi_get_context(void);
extern void _glapi_set_context(void *context);
extern void *_glapi_get_dispatch(void);
extern void _glapi_set_dispatch(void *dispatch);

typedef struct {
    bool bound;
    EGLSurface draw, read;
    EGLContext guest_ctx;   // handle the engine knows this binding by

    bool has_state;
    GLint active_texture, tex_2d[CTX_TEXTURE_UNITS], tex_cube[CTX_TEXTURE_UNITS];
    GLint array_buffer, element_buffer, program, framebuffer, renderbuffer;
    GLint unpack_alignment, pack_alignment;
} thread_binding;

// Heap-allocated and never freed, so s_active stays valid after its thread
// has exited.
static __thread thread_binding *t_binding;
static __thread bool t_attached;      // glapi pointers set on this thread

static EGLContext s_real_ctx = EGL_NO_CONTEXT;
static void *s_glapi_context, *s_glapi_dispatch;
static thread_binding *s_egl_binding; // binding the real context is EGL-current for
static thread_binding *s_active;      // binding whose state the real context holds

static void save_state(thread_binding *b) {
    glGetIntegerv(GL_ACTIVE_TEXTURE, &b->active_texture);
    for (int i = 0; i < CTX_TEXTURE_UNITS; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &b->tex_2d[i]);
        glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &b->tex_cube[i]);
    }
    glActiveTexture((GLenum)b->active_texture);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &b->array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &b->element_buffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &b->program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &b->framebuffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &b->renderbuffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &b->unpack_alignment);
    glGetIntegerv(GL_PACK_ALIGNMENT, &b->pack_alignment);
    b->has_state = true;
}

static void restore_state(thread_binding *b) {
    if (!b->has_state) {
        // A context the engine has not used yet: GL defaults.
        memset(b->tex_2d, 0, sizeof(b->tex_2d));
        memset(b->tex_cube, 0, sizeof(b->tex_cube));
        b->active_texture = GL_TEXTURE0;
        b->array_buffer = b->element_buffer = b->program = b->framebuffer = b->renderbuffer = 0;
        b->unpack_alignment = b->pack_alignment = 4;
        b->has_state = true;
    }
    for (int i = 0; i < CTX_TEXTURE_UNITS; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, (GLuint)b->tex_2d[i]);
        glBindTexture(GL_TEXTURE_CUBE_MAP, (GLuint)b->tex_cube[i]);
    }
    glActiveTexture((GLenum)b->active_texture);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)b->array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)b->element_buffer);
    glUseProgram((GLuint)b->program);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)b->framebuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)b->renderbuffer);
    glPixelStorei(GL_UNPACK_ALIGNMENT, b->unpack_alignment);
    glPixelStorei(GL_PACK_ALIGNMENT, b->pack_alignment);
}

static void attach_thread(void) {
    if (t_attached) return;
    _glapi_set_context(s_glapi_context);
    _glapi_set_dispatch(s_glapi_dispatch);
    t_attached = true;
}

// Caller holds the GL lock and is about to make GL calls: make the real
// context reachable from this thread and give it this thread's bindings.
void egl_ctx_acquire(void) {
    thread_binding *me = t_binding;
    if (!me || !me->bound || s_active == me || !s_glapi_context) return;

    attach_thread();
    if (s_active) save_state(s_active);
    restore_state(me);
    s_active = me;
}

// Caller holds the GL lock.
bool egl_on_drawing_thread(void) {
    return t_binding && s_egl_binding == t_binding;
}

EGLContext wrap_eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                                 const EGLint *attrib_list) {
    for (const EGLint *a = attrib_list; a && *a != EGL_NONE; a += 2)
        l_info("[EGL] eglCreateContext attrib 0x%04x = %d", a[0], a[1]);

    if (s_real_ctx != EGL_NO_CONTEXT) {
        // Any distinct non-null handle will do; it is never passed to Mesa.
        EGLContext alias = (EGLContext)malloc(1);
        l_info("[EGL] eglCreateContext -> %p, alias of %p (share %p)", alias, s_real_ctx, share_context);
        return alias;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, attrib_list);
    if (ctx == EGL_NO_CONTEXT) {
        l_error("[EGL] eglCreateContext failed: 0x%x", eglGetError());
        return ctx;
    }
    l_info("[EGL] eglCreateContext -> %p (share %p)", ctx, share_context);
    s_real_ctx = ctx;
    return ctx;
}

EGLBoolean wrap_eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    if (ctx == EGL_NO_CONTEXT) return EGL_FALSE;
    if (ctx != s_real_ctx) {
        free(ctx);
        return EGL_TRUE;
    }
    s_real_ctx = EGL_NO_CONTEXT;
    s_glapi_context = s_glapi_dispatch = NULL;
    s_egl_binding = NULL;
    s_active = NULL;
    return eglDestroyContext(dpy, ctx);
}

EGLContext wrap_eglGetCurrentContext(void) {
    return (t_binding && t_binding->bound) ? t_binding->guest_ctx : EGL_NO_CONTEXT;
}

// Makes the real context current through EGL, which only the thread that
// owns the window surface needs; everyone else is attached through glapi.
static bool bind_real_context(thread_binding *me, EGLDisplay dpy, EGLSurface draw, EGLSurface read) {
    if (!eglMakeCurrent(dpy, draw, read, s_real_ctx)) {
        l_error("[EGL] eglMakeCurrent(%p, %p) failed: 0x%x", draw, read, eglGetError());
        return false;
    }
    s_glapi_context = _glapi_get_context();
    s_glapi_dispatch = _glapi_get_dispatch();
    t_attached = true;

    if (draw != EGL_NO_SURFACE) {
        s_egl_binding = me;
    } else {
        // Leave EGL free for the drawing thread; keep using the context here.
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        t_attached = false;
        attach_thread();
    }
    return true;
}

EGLBoolean wrap_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    // The engine releases the context with EGL_NO_DISPLAY before it has one.
    if (dpy == EGL_NO_DISPLAY && ctx == EGL_NO_CONTEXT) return EGL_TRUE;
    if (draw == FAKE_PBUFFER) draw = EGL_NO_SURFACE;
    if (read == FAKE_PBUFFER) read = EGL_NO_SURFACE;

    thread_binding *me = t_binding;
    if (ctx == EGL_NO_CONTEXT) {
        if (me) {
            if (s_active == me) {
                save_state(me);
                s_active = NULL;
            }
            me->bound = false;
            me->guest_ctx = EGL_NO_CONTEXT;
            if (s_egl_binding == me) {
                // Really let go of the window surface: the engine destroys
                // and recreates it, and Mesa keeps a current surface alive.
                eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                s_egl_binding = NULL;
                t_attached = false;
            }
        }
        return EGL_TRUE;
    }

    if (!me) {
        me = t_binding = (thread_binding *)calloc(1, sizeof(thread_binding));
        if (!me) return EGL_FALSE;
    }

    bool new_surface = draw != EGL_NO_SURFACE && (s_egl_binding != me || me->draw != draw || me->read != read);
    if (!s_glapi_context || new_surface) {
        if (!bind_real_context(me, dpy, draw, read)) return EGL_FALSE;
    }

    if (me->guest_ctx != ctx) me->has_state = false;
    me->bound = true;
    me->draw = draw;
    me->read = read;
    me->guest_ctx = ctx;
    egl_ctx_acquire();

    l_debug("[EGL] eglMakeCurrent ctx=%p draw=%p on tid %d", ctx, draw, pthr_gettid());
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        l_info("[EGL] context current. GL_VERSION: %s, GL_RENDERER: %s",
               glGetString(GL_VERSION), glGetString(GL_RENDERER));
    }
    return EGL_TRUE;
}

EGLBoolean wrap_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
    EGLBoolean ok = eglGetConfigAttrib(dpy, config, attribute, value);

    // glf only accepts a config whose channel sizes equal the ones it asked
    // for (RGB565 by default), while Mesa on Switch exposes RGBA8888 only.
    // An 8888 surface is a superset, so report the requested sizes.
    if (ok && value) {
        int channel = -1;
        switch (attribute) {
            case EGL_RED_SIZE:   channel = 0; break;
            case EGL_GREEN_SIZE: channel = 1; break;
            case EGL_BLUE_SIZE:  channel = 2; break;
            case EGL_ALPHA_SIZE: channel = 3; break;
        }
        if (channel >= 0 && s_requested_rgba[channel] >= 0 && *value >= s_requested_rgba[channel])
            *value = s_requested_rgba[channel];
    }
    l_debug("[EGL] eglGetConfigAttrib(%p, 0x%04x) -> ok=%d value=0x%x", config, attribute, ok, value ? *value : 0);
    return ok;
}

EGLBoolean wrap_eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    EGLint ma = 0, mi = 0;
    EGLBoolean ok = eglInitialize(dpy, &ma, &mi);
    if (major) *major = ma;
    if (minor) *minor = mi;
    l_info("[EGL] eglInitialize -> ok=%d (EGL %d.%d)", ok, ma, mi);
    return ok;
}

EGLBoolean wrap_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    unsigned long n = __atomic_add_fetch(&s_frames, 1, __ATOMIC_RELAXED);
    if (surface == FAKE_PBUFFER) return EGL_TRUE;
#if AIRBORNE_DEBUG
    gldiag_frame(n);
#endif
    gltrace_frame(n);
    fpsoverlay_frame();
    PROF_BEGIN();
    EGLBoolean ok = eglSwapBuffers(dpy, surface);
    PROF_END(PROF_SWAP);
    prof_frame();
    if (n <= 3 || (n % 600) == 0)
        l_info("[EGL] eglSwapBuffers frame %lu (surface %p) -> %d", n, surface, ok);
    if (!ok) svcSleepThread(16000000ULL); // don't spin if the engine retries
    return ok;
}
