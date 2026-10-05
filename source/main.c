#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "so_util/so_util.h"
#include "java.h"
#include "dynlib.h"
#include "patch.h"
#include "utils/init.h"
#include "utils/logger.h"
#include "reimpl/egl.h"
#include "reimpl/controls.h"
#include "reimpl/fpsoverlay.h"
#include "reimpl/opensles.h"
#include "reimpl/pthr.h"
#include "reimpl/io.h"

#define JNI_PREFIX "Java_com_gameloft_android_ANMP_GloftA8HM_"

// Pinned to the v4.0.0l arm64-v8a libmyAndroid.so: global holding the glf::App*
// that the JNIBridge surface/lifecycle entry points dereference.
#define A8_400L_GLF_APP_SLOT 0x2906BD8

// SO Modules
static so_module so_libcxx;
static so_module so_game;

// Where libmyAndroid.so is loaded (0 before that), for code that reads the
// game's own data.
uintptr_t game_text_base(void) {
    return so_game.base_addr;
}

// JNI Function Signatures
typedef int (*jni_on_load_fn)(void *jvm, void *reserved);
typedef void (*jni_void_fn)(void *env, void *clazz);
typedef void (*jni_int_fn)(void *env, void *clazz, int value);
typedef void (*jni_bool_fn)(void *env, void *clazz, unsigned char value);
typedef void (*jni_surface_fn)(void *env, void *clazz, void *surface, int width, int height);

static uintptr_t required_symbol(so_module *mod, const char *name) {
    uintptr_t addr = so_symbol(mod, name);
    if (!addr) {
        fatal_error("Asphalt 8 missing required symbol: %s", name);
    }
    return addr;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    init_nx();
    l_info("AirborneNX " AIRBORNE_VERSION ": Asphalt 8 port for Nintendo Switch starting...");

    controls_init();
    fpsoverlay_init();
    opensles_init();
    java_init();

    // 1. Load C++ Runtime (.so)
    l_info("Loading libc++_shared.so...");
    if (so_load(&so_libcxx, LIBCPP_SO_PATH) != 0) {
        fatal_error("Failed to load %s. Ensure game files are in %s", LIBCPP_SO_PATH, DATA_PATH);
    }
    resolve_dynamic_dependencies(&so_libcxx);
    so_initialize(&so_libcxx);

    // 2. Load Main Game Library
    l_info("Loading libmyAndroid.so...");
    if (so_load(&so_game, SO_PATH) != 0) {
        fatal_error("Failed to load %s. Ensure game files are in %s", SO_PATH, DATA_PATH);
    }
    resolve_dynamic_dependencies(&so_game);
    so_patch(&so_game);
    so_initialize(&so_game);
    l_info("libmyAndroid.so initialized successfully");

    // 3. Resolve JNI Entry Points
    jni_on_load_fn jni_on_load = (jni_on_load_fn)required_symbol(&so_game, "JNI_OnLoad");
    jni_void_fn bridge_native_init = (jni_void_fn)required_symbol(&so_game,
        JNI_PREFIX "PackageUtils_JNIBridge_NativeInit");
    jni_void_fn bridge_on_resume = (jni_void_fn)required_symbol(&so_game,
        JNI_PREFIX "PackageUtils_JNIBridge_NativeOnResume");
    jni_surface_fn bridge_surface_changed = (jni_surface_fn)required_symbol(&so_game,
        JNI_PREFIX "PackageUtils_JNIBridge_NativeSurfaceChanged");
    jni_int_fn set_gles_version = (jni_int_fn)required_symbol(&so_game,
        JNI_PREFIX "MainActivity_setOpenGlesVersion");
    jni_bool_fn set_safe_zone = (jni_bool_fn)so_symbol(&so_game,
        JNI_PREFIX "GL2JNILib_nativeSetStatusSafeZone");
    jni_bool_fn set_chromebook = (jni_bool_fn)so_symbol(&so_game,
        JNI_PREFIX "GL2JNILib_nativeSetStatusChromeBook");
    jni_void_fn gl2_init = (jni_void_fn)required_symbol(&so_game,
        JNI_PREFIX "GL2JNILib_init");

    controls_resolve(&so_game);

    // 4. Startup sequence, in the order v4.0.0l's MainActivity drives it:
    //    System.loadLibrary -> JNIBridge.NativeInit -> setOpenGlesVersion ->
    //    GL2JNILib.init -> (onResume) NativeOnResume -> (surfaceChanged)
    //    NativeSurfaceChanged. From there the game runs its own render thread
    //    and owns the EGL context; nothing here drives frames.
    int jni_ver = jni_on_load(&jvm, NULL);
    l_info("JNI_OnLoad returned: 0x%x", jni_ver);

    l_info("JNIBridge.NativeInit...");
    bridge_native_init(&jni, NULL);

    if (set_safe_zone) set_safe_zone(&jni, NULL, JNI_FALSE);
    if (set_chromebook) set_chromebook(&jni, NULL, JNI_FALSE);

    l_info("MainActivity.setOpenGlesVersion(2)...");
    set_gles_version(&jni, NULL, 2);

    l_info("GL2JNILib.init...");
    gl2_init(&jni, NULL);

    // GL2JNILib/NativeInit spawn the game thread, which builds the glf::App
    // object asynchronously. On Android the surface shows up long after that;
    // delivering it earlier writes the event into a null App and loses it.
    void **app_slot = (void **)(so_game.base_addr + A8_400L_GLF_APP_SLOT);
    for (int waited_ms = 0; !__atomic_load_n(app_slot, __ATOMIC_ACQUIRE); waited_ms += 10) {
        if (waited_ms >= 30000) fatal_error("Game thread never created its App object");
        svcSleepThread(10000000ULL);
    }
    l_info("Game App object ready: %p", *app_slot);

    l_info("JNIBridge.NativeOnResume...");
    bridge_on_resume(&jni, NULL);

    l_info("JNIBridge.NativeSurfaceChanged(%dx%d)...",
           ASPHALT8_RENDER_WIDTH_DEFAULT, ASPHALT8_RENDER_HEIGHT_DEFAULT);
    jobject surface = jni->NewStringUTF(&jni, "android.view.Surface");
    bridge_surface_changed(&jni, NULL, surface,
                           ASPHALT8_RENDER_WIDTH_DEFAULT, ASPHALT8_RENDER_HEIGHT_DEFAULT);

    l_info("Asphalt 8 lifecycle active! Entering main loop...");

    // 5. Host loop: input and applet events only.
#if AIRBORNE_DEBUG
    u64 last_report = armGetSystemTick();
    unsigned long last_frames = 0;
#endif
    while (appletMainLoop()) {
        controls_poll();
        svcSleepThread(8000000ULL); // 8 ms

#if AIRBORNE_DEBUG
        u64 now = armGetSystemTick();
        if (armTicksToNs(now - last_report) >= 5000000000ULL) {
            unsigned long frames = egl_frame_count();
            unsigned long rd_calls, rd_bytes, rd_ipc;
            io_read_stats(&rd_calls, &rd_bytes, &rd_ipc);
            l_info("heartbeat: %lu frames presented (+%lu in 5s); reads: %lu calls, %lu KB, %lu fs round trips",
                   frames, frames - last_frames, rd_calls, rd_bytes / 1024, rd_ipc);
            static int s_stalled = 0;
            if (frames == last_frames && frames > 0) {
                unsigned secs = 0;
                const char *inflight = io_inflight(&secs);
                if (inflight) l_info("in-flight file call for %us: %s", secs, inflight);
                if ((++s_stalled % 3) == 0 && s_stalled <= 9) {
                    extern void _start(void);
                    l_info("frames stalled; host base=%p game=%p libc++=%p",
                           (void *)_start, (void *)so_game.base_addr, (void *)so_libcxx.base_addr);
                    pthr_dump_threads(so_game.base_addr, so_game.total_size);
                }
            } else {
                s_stalled = 0;
            }
            last_frames = frames;
            last_report = now;
        }
#endif
    }

    l_info("Exiting AirborneNX...");
    cleanup_nx();
    return 0;
}
