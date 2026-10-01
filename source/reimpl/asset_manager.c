#include "asset_manager.h"
#include "../utils/init.h"
#include "../utils/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

struct AAssetManager {
    char base_path[256];
};

struct AAssetDir {
    DIR *dir;
    struct dirent *entry;
    char path[256];
};

struct AAsset {
    FILE *fp;
    size_t length;
    void *buffer;
};

static AAssetManager default_manager;

AAssetManager *AAssetManager_fromJava(void *env, void *assetManager) {
    (void)env;
    (void)assetManager;
    snprintf(default_manager.base_path, sizeof(default_manager.base_path), "%s", ASSETS_PATH);
    return &default_manager;
}

AAssetDir *AAssetManager_openDir(AAssetManager *mgr, const char *dirName) {
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s%s", mgr ? mgr->base_path : ASSETS_PATH, dirName ? dirName : "");

    DIR *d = opendir(full_path);
    if (!d) return NULL;

    AAssetDir *assetDir = (AAssetDir *)malloc(sizeof(AAssetDir));
    assetDir->dir = d;
    assetDir->entry = NULL;
    snprintf(assetDir->path, sizeof(assetDir->path), "%s", full_path);
    return assetDir;
}

const char *AAssetDir_getNextFileName(AAssetDir *assetDir) {
    if (!assetDir || !assetDir->dir) return NULL;
    assetDir->entry = readdir(assetDir->dir);
    if (!assetDir->entry) return NULL;
    return assetDir->entry->d_name;
}

void AAssetDir_rewind(AAssetDir *assetDir) {
    if (assetDir && assetDir->dir) {
        rewinddir(assetDir->dir);
    }
}

void AAssetDir_close(AAssetDir *assetDir) {
    if (assetDir) {
        if (assetDir->dir) closedir(assetDir->dir);
        free(assetDir);
    }
}

AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode) {
    (void)mode;
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s%s", mgr ? mgr->base_path : ASSETS_PATH, filename ? filename : "");

    FILE *f = fopen(full_path, "rb");
    l_debug("[asset] open %s%s", f ? "" : "FAILED ", full_path);
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    fseek(f, 0, SEEK_SET);

    AAsset *asset = (AAsset *)malloc(sizeof(AAsset));
    asset->fp = f;
    asset->length = len;
    asset->buffer = NULL;
    return asset;
}

int AAsset_read(AAsset *asset, void *buf, size_t count) {
    if (!asset || !asset->fp) return -1;
    return fread(buf, 1, count, asset->fp);
}

off_t AAsset_seek(AAsset *asset, off_t offset, int whence) {
    if (!asset || !asset->fp) return -1;
    fseek(asset->fp, offset, whence);
    return ftell(asset->fp);
}

void AAsset_close(AAsset *asset) {
    if (asset) {
        if (asset->fp) fclose(asset->fp);
        if (asset->buffer) free(asset->buffer);
        free(asset);
    }
}

const void *AAsset_getBuffer(AAsset *asset) {
    if (!asset || !asset->fp) return NULL;
    if (!asset->buffer) {
        asset->buffer = malloc(asset->length);
        if (asset->buffer) {
            long cur = ftell(asset->fp);
            fseek(asset->fp, 0, SEEK_SET);
            fread(asset->buffer, 1, asset->length, asset->fp);
            fseek(asset->fp, cur, SEEK_SET);
        }
    }
    return asset->buffer;
}

off_t AAsset_getLength(AAsset *asset) {
    return asset ? asset->length : 0;
}

off_t AAsset_getRemainingLength(AAsset *asset) {
    if (!asset || !asset->fp) return 0;
    long cur = ftell(asset->fp);
    return (asset->length > (size_t)cur) ? (asset->length - cur) : 0;
}

// ANativeWindow stubs
struct ANativeWindow {
    int width;
    int height;
};

static ANativeWindow g_dummy_window = {
    .width = ASPHALT8_RENDER_WIDTH_DEFAULT,
    .height = ASPHALT8_RENDER_HEIGHT_DEFAULT
};

ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface) {
    (void)env;
    (void)surface;
    return &g_dummy_window;
}

void ANativeWindow_release(ANativeWindow *window) {
    (void)window;
}

int32_t ANativeWindow_getWidth(ANativeWindow *window) {
    return window ? window->width : ASPHALT8_RENDER_WIDTH_DEFAULT;
}

int32_t ANativeWindow_getHeight(ANativeWindow *window) {
    return window ? window->height : ASPHALT8_RENDER_HEIGHT_DEFAULT;
}

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width, int32_t height, int32_t format) {
    (void)format;
    if (window) {
        window->width = width;
        window->height = height;
    }
    return 0;
}
