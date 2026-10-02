#include "shadercache.h"
#include "../utils/init.h"
#include "../utils/logger.h"
#include <switch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ============================================================================
// Shader program cache.
//
// The engine builds every program from GLSL source each time it needs it, and
// on this driver that is 30 to 60 ms per program on the thread that draws: a
// visible hitch the first time anything new appears on screen, on every run.
// The driver can hand back a linked program as a binary blob and take it
// again later, so each program is built once, stored on the SD card under a
// hash of its sources, and loaded from there afterwards.
//
// Compiling is put off until a program is linked. On a cache hit the shaders
// are never compiled at all; the engine is told they compiled fine, which is
// what happens anyway with shaders that shipped in the game.
//
// Everything here runs under the GL lock (these are GL imports), so nothing
// needs a lock of its own.
// ============================================================================
#define SC_FILE        CACHE_PATH "shaders.bin"
#define SC_MAGIC       "A8NXSC01"
#define SC_IDS         8192     // shader / program names tracked; beyond that, no caching
#define SC_ATTACHED    4
#define SC_REJECT_MAX  8        // binaries the driver may refuse before the cache is given up

#define GL_PROGRAM_BINARY_LENGTH_       0x8741
#define GL_NUM_PROGRAM_BINARY_FORMATS_  0x87FE

typedef struct {
    uint64_t hash;      // of the source text
    bool pending;       // glCompileShader was asked for and not done yet
} shader_info;

typedef struct {
    GLuint shaders[SC_ATTACHED];
    int count;
    uint64_t bindings;  // of the glBindAttribLocation calls, in any order
} program_info;

typedef struct {
    uint64_t key;
    uint32_t format, length;
} record_header;

typedef struct {
    uint64_t key;
    uint32_t format, length;
    const uint8_t *data;
} cache_entry;

static shader_info s_shaders[SC_IDS];
static program_info s_programs[SC_IDS];

static bool s_ready, s_enabled;
static FILE *s_file;                // open for appending new programs
static uint8_t *s_image;            // the cache file as it was at start-up
static cache_entry *s_entries;
static size_t s_entry_count, s_entry_room;
static unsigned s_rejected, s_loaded, s_built;

static uint64_t fnv64(uint64_t hash, const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++) hash = (hash ^ p[i]) * 1099511628211ULL;
    return hash;
}
#define FNV64_SEED 14695981039346656037ULL

void shadercache_stats(unsigned *loaded, unsigned *built) {
    *loaded = s_loaded;
    *built = s_built;
    s_loaded = s_built = 0;
}

// "shadercache=0" in config.ini turns the cache off without a rebuild.
static bool configured_on(void) {
    int on = 1;
    FILE *f = fopen(DATA_PATH "config.ini", "r");
    if (!f) return true;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int value = 0;
        if (sscanf(line, " shadercache = %d", &value) == 1) on = value;
    }
    fclose(f);
    return on != 0;
}

static void remember(uint64_t key, uint32_t format, uint32_t length, const uint8_t *data) {
    for (size_t i = 0; i < s_entry_count; i++) {
        if (s_entries[i].key != key) continue;
        s_entries[i].format = format;
        s_entries[i].length = length;
        s_entries[i].data = data;
        return;
    }
    if (s_entry_count == s_entry_room) {
        size_t room = s_entry_room ? s_entry_room * 2 : 256;
        cache_entry *grown = (cache_entry *)realloc(s_entries, room * sizeof(cache_entry));
        if (!grown) return;
        s_entries = grown;
        s_entry_room = room;
    }
    s_entries[s_entry_count++] = (cache_entry){ key, format, length, data };
}

// The file starts with the magic and a hash of what built its binaries; they
// are only good for the same driver and the same build of the port.
static uint64_t driver_id(void) {
    uint64_t id = FNV64_SEED;
    const char *parts[] = { (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
                            AIRBORNE_VERSION };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        if (parts[i]) id = fnv64(id, parts[i], strlen(parts[i]) + 1);
    return id;
}

// Mesa only lets program binaries out when the driver has an on-disk shader
// cache of its own. The Switch build has none, which leaves two things wrong
// although the code that makes and reads the binaries is all there:
//
//  - the context reports no binary formats, and glGetProgramBinary refuses;
//  - the function that stamps each binary with the driver's identity is
//    empty, so the stamp is whatever was on the stack and never matches when
//    the binary comes back.
//
// Both are one field of the context each: the format count is set to 1, and
// the identity function is replaced by one that writes a fixed stamp. The
// offsets are those of the Mesa this port links against (read from
// _mesa_GetProgramBinary and _mesa_program_binary), so nothing is touched
// unless it is that exact version, the fields hold what that version puts
// there, and the driver then really reports the format.
#define MESA_VERSION_KNOWN          "OpenGL ES 3.2 Mesa 20.1.0-rc3"
#define MESA_BINARY_FORMATS_OFFSET  0x1130C // gl_context.Const.NumProgramBinaryFormats
#define MESA_DRIVER_SHA1_OFFSET     0x109C8 // gl_context.Driver.GetProgramBinaryDriverSHA1

typedef void (*driver_sha1_fn)(void *ctx, uint8_t *sha1);

extern void *_glapi_get_context(void);
extern void st_get_program_binary_driver_sha1(void *ctx, uint8_t *sha1);

static void airborne_driver_sha1(void *ctx, uint8_t *sha1) {
    (void)ctx;
    static const char stamp[20] = "AirborneNX shaders 1";
    memcpy(sha1, stamp, 20);
}

static GLint binary_formats(void) {
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS_, &formats);
    if (formats > 0) return formats;

    const char *version = (const char *)glGetString(GL_VERSION);
    char *context = (char *)_glapi_get_context();
    if (!context || !version || strcmp(version, MESA_VERSION_KNOWN) != 0) return 0;

    uint32_t *count = (uint32_t *)(context + MESA_BINARY_FORMATS_OFFSET);
    driver_sha1_fn *stamp = (driver_sha1_fn *)(context + MESA_DRIVER_SHA1_OFFSET);
    if (*count != 0 || *stamp != st_get_program_binary_driver_sha1) return 0;

    *count = 1;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS_, &formats);
    if (formats != 1) {
        *count = 0;
        return 0;
    }
    *stamp = airborne_driver_sha1;
    return formats;
}

// Called at the first link, with the context current.
static void cache_open(void) {
    s_ready = true;

    GLint formats = configured_on() ? binary_formats() : -1;
    if (formats <= 0) {
        l_info("[shaders] cache off (%s)", formats == 0 ? "the driver has no program binaries" : "config.ini");
        return;
    }

    uint64_t id = driver_id();
    long valid = 0;
    FILE *f = fopen(SC_FILE, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size >= 16 && (s_image = (uint8_t *)malloc((size_t)size)) != NULL &&
            fread(s_image, 1, (size_t)size, f) == (size_t)size &&
            memcmp(s_image, SC_MAGIC, 8) == 0 && memcmp(s_image + 8, &id, 8) == 0) {
            // Stop at the first record that is not whole: the console may
            // have been switched off in the middle of a write.
            valid = 16;
            while (valid + (long)sizeof(record_header) <= size) {
                record_header h;
                memcpy(&h, s_image + valid, sizeof(h));
                if (h.length == 0 || h.length > (size_t)(size - valid) - sizeof(h)) break;
                remember(h.key, h.format, h.length, s_image + valid + sizeof(h));
                valid += (long)(sizeof(h) + h.length);
            }
        }
        fclose(f);
    }

    if (valid) {
        s_file = fopen(SC_FILE, "r+b");
        if (s_file) fseek(s_file, valid, SEEK_SET);
    } else {
        // No cache yet, or one from another driver or build: start over.
        free(s_image);
        s_image = NULL;
        s_entry_count = 0;
        mkdir(DATA_PATH "cache", 0777);
        s_file = fopen(SC_FILE, "wb");
        if (s_file) {
            fwrite(SC_MAGIC, 1, 8, s_file);
            fwrite(&id, 1, 8, s_file);
            fflush(s_file);
        }
    }
    s_enabled = true;
    l_info("[shaders] cache on: %zu program(s) stored%s", s_entry_count,
           s_file ? "" : ", cannot write new ones");
}

static void store(GLuint program, uint64_t key) {
    GLint length = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH_, &length);
    if (length <= 0) return;
    uint8_t *data = (uint8_t *)malloc((size_t)length);
    if (!data) return;

    GLenum format = 0;
    GLsizei written = 0;
    glGetProgramBinary(program, length, &written, &format, data);
    if (written <= 0) {
        free(data);
        return;
    }
    remember(key, format, (uint32_t)written, data); // kept for the rest of the run

    if (!s_file) return;
    record_header h = { key, format, (uint32_t)written };
    if (fwrite(&h, 1, sizeof(h), s_file) != sizeof(h) ||
        fwrite(data, 1, (size_t)written, s_file) != (size_t)written || fflush(s_file) != 0) {
        l_warn("[shaders] cannot write to the cache file; new programs will not be kept");
        fclose(s_file);
        s_file = NULL;
    }
}

// True if `program` is now linked from a stored binary.
static bool load(GLuint program, uint64_t key) {
    for (size_t i = 0; i < s_entry_count; i++) {
        if (s_entries[i].key != key) continue;

        glProgramBinary(program, s_entries[i].format, s_entries[i].data, (GLsizei)s_entries[i].length);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked) return true;

        // Refused: the engine must not find the error this left behind.
        for (int n = 0; n < 8 && glGetError() != GL_NO_ERROR; n++) {}
        if (++s_rejected >= SC_REJECT_MAX) {
            l_warn("[shaders] the driver keeps refusing stored programs; cache off");
            s_enabled = false;
        }
        return false;
    }
    return false;
}

static bool tracked(GLuint name) {
    return name > 0 && name < SC_IDS;
}

GLuint sc_glCreateShader(GLenum type) {
    GLuint shader = glCreateShader(type);
    if (tracked(shader)) memset(&s_shaders[shader], 0, sizeof(shader_info));
    return shader;
}

GLuint sc_glCreateProgram(void) {
    GLuint program = glCreateProgram();
    if (tracked(program)) memset(&s_programs[program], 0, sizeof(program_info));
    return program;
}

void sc_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
    glShaderSource(shader, count, string, length);
    if (!tracked(shader)) return;

    uint64_t hash = FNV64_SEED;
    for (GLsizei i = 0; i < count; i++) {
        if (!string || !string[i]) continue;
        size_t size = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
        hash = fnv64(hash, string[i], size);
    }
    s_shaders[shader].hash = hash;
    s_shaders[shader].pending = false;
}

void sc_glCompileShader(GLuint shader) {
#if AIRBORNE_DEBUG
    glCompileShader(shader); // debug builds report compile errors as they happen
#else
    if (tracked(shader)) s_shaders[shader].pending = true;
    else glCompileShader(shader);
#endif
}

static void compile_now(GLuint shader) {
    if (!tracked(shader) || !s_shaders[shader].pending) return;
    s_shaders[shader].pending = false;
    glCompileShader(shader);
}

void sc_glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    if (tracked(shader) && s_shaders[shader].pending && params) {
        if (pname == GL_COMPILE_STATUS) { *params = GL_TRUE; return; }
        if (pname == GL_INFO_LOG_LENGTH) { *params = 0; return; }
    }
    glGetShaderiv(shader, pname, params);
}

void sc_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    if (tracked(shader) && s_shaders[shader].pending) {
        if (length) *length = 0;
        if (infoLog && bufSize > 0) infoLog[0] = 0;
        return;
    }
    glGetShaderInfoLog(shader, bufSize, length, infoLog);
}

void sc_glAttachShader(GLuint program, GLuint shader) {
    glAttachShader(program, shader);
    program_info *p = tracked(program) ? &s_programs[program] : NULL;
    if (!p || p->count >= SC_ATTACHED) {
        // Not followed from here on, so it cannot wait for the link.
        compile_now(shader);
        if (!p) return;
    } else {
        p->shaders[p->count] = shader;
    }
    p->count++; // past SC_ATTACHED the program is simply not cached
}

void sc_glDetachShader(GLuint program, GLuint shader) {
    glDetachShader(program, shader);
    if (!tracked(program)) return;
    program_info *p = &s_programs[program];
    for (int i = 0; i < p->count && i < SC_ATTACHED; i++) {
        if (p->shaders[i] != shader) continue;
        for (int k = i; k + 1 < p->count && k + 1 < SC_ATTACHED; k++) p->shaders[k] = p->shaders[k + 1];
        p->count--;
        return;
    }
}

void sc_glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) {
    glBindAttribLocation(program, index, name);
    if (!tracked(program) || !name) return;
    // Summed so that the order of the calls does not matter.
    s_programs[program].bindings += fnv64(fnv64(FNV64_SEED, &index, sizeof(index)), name, strlen(name));
}

// Hash of everything the linked program depends on; false if it is not known.
static bool program_key(GLuint program, uint64_t *key) {
    if (!tracked(program)) return false;
    const program_info *p = &s_programs[program];
    if (p->count < 1 || p->count > SC_ATTACHED) return false;

    // Sorted, so the order the shaders were attached in does not matter.
    uint64_t hashes[SC_ATTACHED];
    for (int i = 0; i < p->count; i++) {
        if (!tracked(p->shaders[i]) || !s_shaders[p->shaders[i]].hash) return false;
        hashes[i] = s_shaders[p->shaders[i]].hash;
    }
    for (int i = 1; i < p->count; i++)
        for (int k = i; k > 0 && hashes[k] < hashes[k - 1]; k--) {
            uint64_t swap = hashes[k];
            hashes[k] = hashes[k - 1];
            hashes[k - 1] = swap;
        }

    uint64_t hash = fnv64(FNV64_SEED, hashes, sizeof(uint64_t) * (size_t)p->count);
    *key = fnv64(hash, &p->bindings, sizeof(p->bindings));
    return true;
}

void sc_glLinkProgram(GLuint program) {
    if (!s_ready) cache_open();

    uint64_t key = 0;
    bool keyed = s_enabled && program_key(program, &key);
    if (keyed && load(program, key)) {
        s_loaded++;
        return;
    }

    if (tracked(program)) {
        const program_info *p = &s_programs[program];
        for (int i = 0; i < p->count && i < SC_ATTACHED; i++) compile_now(p->shaders[i]);
    }
    glLinkProgram(program);
    s_built++;

    if (!keyed || !s_enabled) return;
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked) store(program, key);
}
