#ifndef __REIMPL_ASSET_MANAGER_H__
#define __REIMPL_ASSET_MANAGER_H__

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AAssetManager AAssetManager;
typedef struct AAssetDir AAssetDir;
typedef struct AAsset AAsset;

enum {
    AASSET_MODE_UNKNOWN   = 0,
    AASSET_MODE_RANDOM    = 1,
    AASSET_MODE_STREAMING = 2,
    AASSET_MODE_BUFFER    = 3
};

AAssetManager *AAssetManager_fromJava(void *env, void *assetManager);
AAssetDir *AAssetManager_openDir(AAssetManager *mgr, const char *dirName);
AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode);
const char *AAssetDir_getNextFileName(AAssetDir *assetDir);
void AAssetDir_rewind(AAssetDir *assetDir);
void AAssetDir_close(AAssetDir *assetDir);
int AAsset_read(AAsset *asset, void *buf, size_t count);
off_t AAsset_seek(AAsset *asset, off_t offset, int whence);
void AAsset_close(AAsset *asset);
const void *AAsset_getBuffer(AAsset *asset);
off_t AAsset_getLength(AAsset *asset);
off_t AAsset_getRemainingLength(AAsset *asset);

// ANativeWindow stubs
typedef struct ANativeWindow ANativeWindow;

ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface);
void ANativeWindow_release(ANativeWindow *window);
int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width, int32_t height, int32_t format);

#ifdef __cplusplus
}
#endif

#endif // __REIMPL_ASSET_MANAGER_H__
