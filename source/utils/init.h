#ifndef __INIT_H__
#define __INIT_H__

// Everything the port reads or writes lives in this folder of the SD card.
#define DATA_ROOT      "sdmc:/switch/AirborneNX"
#define DATA_PATH      DATA_ROOT "/"
#define FILES_PATH     DATA_PATH "files/"
#define INTERNAL_PATH  DATA_PATH "internal/"
#define CACHE_PATH     DATA_PATH "cache/"
#define SO_PATH        DATA_PATH "libmyAndroid.so"
#define LIBCPP_SO_PATH DATA_PATH "libc++_shared.so"

#define ASSETS_PATH    DATA_PATH "assets/"
#define APK_PATH       DATA_PATH "base.apk"
#define SYSROOT_PATH   DATA_PATH "sysroot/"

// Paths as the game sees them. It is handed genuine Android-style paths and
// every file call is redirected by translate_path() (reimpl/io.c).
#define ANDROID_PKG       "com.gameloft.android.ANMP.GloftA8HM"
#define ANDROID_SDCARD    "/sdcard"
#define ANDROID_FILES     "/sdcard/Android/data/" ANDROID_PKG "/files"
#define ANDROID_INTERNAL  "/data/data/" ANDROID_PKG "/files"
#define ANDROID_CACHE     "/data/data/" ANDROID_PKG "/cache"
#define ANDROID_LIBDIR    "/data/app/" ANDROID_PKG "/lib/arm64"
#define ANDROID_APK       "/data/app/" ANDROID_PKG "/base.apk"

#define ASPHALT8_RENDER_WIDTH_DEFAULT  1280
#define ASPHALT8_RENDER_HEIGHT_DEFAULT 720

int init_nx(void);
void cleanup_nx(void);
int ensure_directories(void);

#endif // __INIT_H__
