#include "shopmod.h"
#include "../utils/init.h"
#include "../utils/logger.h"
#include <so_util/so_util.h>
#include <switch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>

// ============================================================================
// Token-only vehicles for credits.
//
// Tokens (the blue currency) come from purchases and online events, so in an
// offline port the 54 vehicles that are sold only for tokens can never be
// had. Their prices live in files/xml/asphaltshop.xtea, an XML file the
// engine decrypts at start-up, where each car has a row like
//
//     <Price Id="CAR_PRICE" Price_SC="0" Price_HC="8500" Price_MP="0" />
//
// (SC = credits, HC = tokens). Vehicles with both prices already exist, and
// the garage then offers both buttons, so a token-only car is given a credit
// price as well: its tokens times a rate. 150 is the game's own exchange: an
// upgrade priced in both currencies costs a median of 140 credits per token,
// and cars priced in both cost 120 to 200.
//
// The game has the prices in two places, and both are converted:
//
//  - files/xml/asphaltshop.xtea, the shop database. The file on the card is
//    never written: a converted copy is made once in the cache folder and the
//    game is pointed at it; it is rebuilt when the original or the settings
//    change.
//  - files/initialfeed.dat, the offline copy of the server's store catalog,
//    which overrides the shop database when present (it also activates the
//    vehicles added after the shop database was made). It is decrypted inside
//    the game and read as JSON, so the JSON reader is hooked instead and the
//    catalog is rewritten on its way in (see below).
//
// Vehicles built from blueprints have a token price too, but the garage shows
// them the blueprint screen, which needs the server, instead of their prices;
// see shopmod_install for how they are sold like the others.
//
// "tokencars=0" in config.ini turns this off and "tokenrate=<n>" changes the
// rate.
//
// File format: bytes 01 00, then XTEA (32 rounds, ECB, little-endian) over
// u32 text length, u32 crc32 of the text, the text, zero padding to 8 bytes.
// The key is a string in libmyAndroid.so folded into 16 bytes, read from the
// loaded game at run time rather than copied here.
// ============================================================================
#define SHOP_NAME        "asphaltshop.xtea"
#define SHOP_SOURCE      FILES_PATH "xml/" SHOP_NAME
#define SHOP_CACHE       CACHE_PATH "asphaltshop_credits.xtea"
#define SHOP_STAMP       CACHE_PATH "asphaltshop_credits.txt"
#define SHOP_FORMAT      5          // bump when the conversion changes
#define DEFAULT_RATE     150
#define XTEA_ROUNDS      32
#define XTEA_DELTA       0x9E3779B9u

// The key string in v4.0.0l, relative to the library's load address, and its
// length; checked before use (see shop_key).
#define A8_400L_SHOP_KEY_OFFSET 0x205a6f2u
#define A8_400L_SHOP_KEY_LENGTH 50

extern uintptr_t game_text_base(void);

static Mutex s_lock;
static int s_state;                 // 0 not decided yet, 1 serve the copy, -1 serve the original
static int s_rate = DEFAULT_RATE;

static bool configured_on(void) {
    int on = 1;
    FILE *f = fopen(DATA_PATH "config.ini", "r");
    if (!f) return true;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int value = 0;
        if (sscanf(line, " tokencars = %d", &value) == 1) on = value;
        if (sscanf(line, " tokenrate = %d", &value) == 1 && value > 0 && value <= 100000) s_rate = value;
    }
    fclose(f);
    return on != 0;
}

static bool shop_key(uint32_t key[4]) {
    const char *s = (const char *)(game_text_base() + A8_400L_SHOP_KEY_OFFSET);
    if (!game_text_base() || strnlen(s, A8_400L_SHOP_KEY_LENGTH + 1) != A8_400L_SHOP_KEY_LENGTH) return false;
    uint8_t folded[16] = { 0 };
    for (int i = 0; i < A8_400L_SHOP_KEY_LENGTH; i++) folded[i & 15] ^= (uint8_t)s[i];
    memcpy(key, folded, 16);
    return true;
}

static void xtea_decrypt(uint32_t *v, size_t words, const uint32_t key[4]) {
    for (size_t i = 0; i + 1 < words; i += 2) {
        uint32_t v0 = v[i], v1 = v[i + 1], sum = XTEA_DELTA * XTEA_ROUNDS;
        for (int r = 0; r < XTEA_ROUNDS; r++) {
            v1 -= (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
            sum -= XTEA_DELTA;
            v0 -= (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        }
        v[i] = v0;
        v[i + 1] = v1;
    }
}

static void xtea_encrypt(uint32_t *v, size_t words, const uint32_t key[4]) {
    for (size_t i = 0; i + 1 < words; i += 2) {
        uint32_t v0 = v[i], v1 = v[i + 1], sum = 0;
        for (int r = 0; r < XTEA_ROUNDS; r++) {
            v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
            sum += XTEA_DELTA;
            v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
        }
        v[i] = v0;
        v[i + 1] = v1;
    }
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) *size = (size_t)n;
    return data;
}

// What the copy was made from; it is only reused while this still matches.
static void stamp_of(const struct stat *source, char *out, size_t size) {
    snprintf(out, size, "format %d rate %d source %lld bytes, modified %lld\n", SHOP_FORMAT, s_rate,
             (long long)source->st_size, (long long)source->st_mtime);
}

static bool copy_is_current(const char *stamp) {
    struct stat st;
    if (stat(SHOP_CACHE, &st) != 0 || st.st_size <= 0) return false;
    char saved[160] = "";
    FILE *f = fopen(SHOP_STAMP, "r");
    if (!f) return false;
    bool ok = fgets(saved, sizeof(saved), f) != NULL && strcmp(saved, stamp) == 0;
    fclose(f);
    return ok;
}

// Rewrites every token-only CAR_PRICE row; returns the new text (malloc'd).
static char *convert(const char *text, size_t length, size_t *out_length, int *changed) {
    static const char row[] = "Id=\"CAR_PRICE\" Price_SC=\"0\" Price_HC=\"";
    size_t row_len = sizeof(row) - 1;
    char *out = (char *)malloc(length + length / 8 + 1024);
    if (!out) return NULL;

    size_t o = 0;
    const char *p = text, *end = text + length;
    *changed = 0;
    for (;;) {
        const char *hit = (const char *)memmem(p, (size_t)(end - p), row, row_len);
        if (!hit) break;
        const char *num = hit + row_len;
        char *after = NULL;
        unsigned long tokens = strtoul(num, &after, 10);
        if (after == num || *after != '"' || tokens == 0) {
            // Not a token price after all: copy through and keep looking.
            memcpy(out + o, p, (size_t)(num - p));
            o += (size_t)(num - p);
            p = num;
            continue;
        }
        memcpy(out + o, p, (size_t)(hit - p));
        o += (size_t)(hit - p);
        o += (size_t)sprintf(out + o, "Id=\"CAR_PRICE\" Price_SC=\"%lu\" Price_HC=\"%lu",
                             tokens * (unsigned long)s_rate, tokens);
        p = after;
        (*changed)++;
    }
    memcpy(out + o, p, (size_t)(end - p));
    o += (size_t)(end - p);
    *out_length = o;
    return out;
}

static bool build_copy(const char *stamp) {
    uint32_t key[4];
    if (!shop_key(key)) {
        l_warn("[shop] key not found in this game build; vehicles keep their prices");
        return false;
    }

    size_t size = 0;
    uint8_t *blob = read_file(SHOP_SOURCE, &size);
    if (!blob || size < 10 || (size - 2) % 8 != 0 || blob[0] != 1 || blob[1] != 0) {
        l_warn("[shop] %s is missing or not in the expected format", SHOP_SOURCE);
        free(blob);
        return false;
    }
    uint8_t *plain = blob + 2;
    xtea_decrypt((uint32_t *)plain, (size - 2) / 4, key);
    uint32_t length, crc;
    memcpy(&length, plain, 4);
    memcpy(&crc, plain + 4, 4);
    if (length > size - 10 || crc32(0, plain + 8, length) != crc) {
        l_warn("[shop] %s did not decrypt correctly; vehicles keep their prices", SHOP_SOURCE);
        free(blob);
        return false;
    }

    int changed = 0;
    size_t new_length = 0;
    char *text = convert((const char *)plain + 8, length, &new_length, &changed);
    free(blob);
    if (!text) return false;

    size_t body = (8 + new_length + 7) & ~(size_t)7;
    uint8_t *out = (uint8_t *)calloc(1, 2 + body);
    if (!out) {
        free(text);
        return false;
    }
    out[0] = 1;
    uint32_t header[2] = { (uint32_t)new_length, (uint32_t)crc32(0, (const uint8_t *)text, new_length) };
    memcpy(out + 2, header, 8);
    memcpy(out + 10, text, new_length);
    free(text);
    xtea_encrypt((uint32_t *)(out + 2), body / 4, key);

    mkdir(DATA_PATH "cache", 0777);
    FILE *f = fopen(SHOP_CACHE, "wb");
    bool ok = f && fwrite(out, 1, 2 + body, f) == 2 + body;
    if (f && fclose(f) != 0) ok = false;
    free(out);
    if (ok && (f = fopen(SHOP_STAMP, "w")) != NULL) {
        ok = fputs(stamp, f) >= 0;
        if (fclose(f) != 0) ok = false;
    }
    if (!ok) {
        remove(SHOP_STAMP);
        l_warn("[shop] cannot write %s; vehicles keep their prices", SHOP_CACHE);
        return false;
    }
    l_info("[shop] shop database: %d token-only vehicles also priced in credits (%d per token)", changed, s_rate);
    return true;
}

// Decides once whether the converted copy is used, building it if needed.
static bool copy_ready(void) {
    mutexLock(&s_lock);
    if (s_state == 0) {
        s_state = -1;
        struct stat source;
        if (configured_on() && stat(SHOP_SOURCE, &source) == 0) {
            char stamp[160];
            stamp_of(&source, stamp, sizeof(stamp));
            if (copy_is_current(stamp)) {
                s_state = 1;
                l_info("[shop] using the converted shop (%d credits per token)", s_rate);
            } else if (build_copy(stamp)) {
                s_state = 1;
            }
        } else {
            l_info("[shop] token-only vehicles keep their prices");
        }
    }
    bool ready = s_state == 1;
    mutexUnlock(&s_lock);
    return ready;
}

const char *shopmod_redirect(const char *path) {
    if (!path) return path;
    size_t len = strlen(path), name_len = sizeof(SHOP_NAME) - 1;
    if (len < name_len || strcasecmp(path + len - name_len, SHOP_NAME) != 0) return path;
    if (len > name_len && path[len - name_len - 1] != '/') return path;
    if (!copy_ready()) return path;
    static int s_logged;
    if (s_logged++ < 4) l_info("[shop] %s served from the converted copy", path);
    return SHOP_CACHE;
}

// ============================================================================
// The store catalog.
//
// initialfeed.dat holds the catalog as JSON; a vehicle sold only for tokens
// reads
//
//     {"_id":"Lamborghini_Veneno___CAR_PRICE","billing_methods":[{"name":
//      "offline","price":[{"currency":"hardcurrency","price":1100}],...
//
// and one sold for both (Suzuki GSX-R750) lists a credits entry before the
// hardcurrency one. The game decrypts the file itself and hands the text to
// jsoncpp, of which it carries three copies; every document goes through
// Reader::parse(begin, end, root, collectComments). That entry is hooked, and
// a document that is the catalog is parsed from a rewritten copy in which
// token-only vehicles get the credits entry as well, and token-only vinyl
// groups ("decals_group") get a credits price in place of the tokens one. Everything else is
// passed through untouched.
//
// The hook needs the four instructions it overwrites to run somewhere before
// jumping back; they are copied into the body of an ads callback that nothing
// in the port ever calls.
// ============================================================================
#define A8_400L_JSON_PARSE_COUNT  3
static const uintptr_t s_parse_offsets[A8_400L_JSON_PARSE_COUNT] = { 0x19c6a6cu, 0x1a5fee8u, 0x1c877dcu };
static const uint32_t s_parse_preimage[4] = {
    0xd101c3ffu, // sub sp, sp, #0x70
    0xf9001bf8u, // str x24, [sp, #0x30]
    0xa9045bf7u, // stp x23, x22, [sp, #0x40]
    0xa90553f5u, // stp x21, x20, [sp, #0x50]
};
// Java_com_gameloft_adsmanager_UnityAdsManager_UnityAdsNotifyEvent, 8676 bytes.
#define A8_400L_SPARE_CODE        0x1f40f8cu
#define A8_400L_SPARE_CODE_WORD0  0xa9ba6ffcu // stp x28, x27, [sp, #-0x60]!
#define TRAMPOLINE_WORDS          8

// crafting_cars::CraftingCarsMgr's "is this vehicle built from blueprints"
// check: a lookup of the car id in the manager's map of craftable cars. The
// garage (and twenty other places) shows the blueprint screen when it says
// yes. The map is filled from more than the catalog's "CRAFTABLE" category,
// so the check itself is made to answer no: the garage then shows the
// vehicle's prices like any other, and buying it works the same way.
#define A8_400L_IS_CRAFTABLE        0x560b90u
#define A8_400L_IS_CRAFTABLE_WORD0  0xf8488c09u // ldr x9, [x0, #0x88]!
#define A8_400L_IS_CRAFTABLE_WORD1  0xb4000269u // cbz x9, ...
#define A64_MOV_W0_0                0x52800000u
#define A64_RET                     0xd65f03c0u

typedef bool (*json_parse_fn)(void *reader, const char *begin, const char *end, void *root, bool collect);
static json_parse_fn s_parse_original[A8_400L_JSON_PARSE_COUNT];

static Mutex s_catalog_lock;
static uint32_t s_catalog_crc;          // last catalog seen, and its rewrite
static size_t s_catalog_length, s_rewritten_length;
static char *s_rewritten;

// Where a token-only price array of one catalog item starts and ends.
typedef struct {
    size_t at, end;
    unsigned long tokens;
    bool vinyl;
} catalog_edit;

typedef struct {
    catalog_edit *edits;
    size_t count, room;
    int vehicles, vinyls;
} catalog_scan;

// One item of the catalog, text[start, end): a vehicle ("___CAR_PRICE") or a
// vinyl group ("decals_group") whose only price is in tokens gets an edit.
static void scan_item(catalog_scan *scan, const char *text, size_t start, size_t end, const char *id, size_t id_len) {
    static const char token_only[] = "\"price\":[{\"currency\":\"hardcurrency\",\"price\":";
    static const char vinyl[] = "\"category\":[\"decals_group\"]";
    static const char credits[] = "\"currency\":\"credits\"";
    const char *item = text + start;
    size_t len = end - start;

    bool vehicle = id_len > 12 && memcmp(id + id_len - 12, "___CAR_PRICE", 12) == 0;
    bool decal = !vehicle && memmem(item, len, vinyl, sizeof(vinyl) - 1) != NULL;
    if (!vehicle && !decal) return;
    if (memmem(item, len, credits, sizeof(credits) - 1)) return; // already sold for credits

    const char *price = (const char *)memmem(item, len, token_only, sizeof(token_only) - 1);
    if (!price) return;
    char *after = NULL;
    unsigned long tokens = strtoul(price + sizeof(token_only) - 1, &after, 10);
    if (tokens == 0 || !after || after + 2 > item + len || after[0] != '}' || after[1] != ']') return;

    if (scan->count == scan->room) {
        size_t room = scan->room ? scan->room * 2 : 256;
        catalog_edit *grown = (catalog_edit *)realloc(scan->edits, room * sizeof(catalog_edit));
        if (!grown) return;
        scan->edits = grown;
        scan->room = room;
    }
    scan->edits[scan->count++] = (catalog_edit){ (size_t)(price - text), (size_t)(after + 2 - text), tokens, decal };
    if (vehicle) scan->vehicles++;
    else scan->vinyls++;
}

// Gives every token-only vehicle a credits price as well, and sells
// token-only vinyl groups for credits instead.
// The items are found by walking the JSON objects: in the catalog the price
// comes before the "_id" inside each item, so an item is only judged once its
// closing brace is reached. NULL if nothing needed changing.
static char *convert_catalog(const char *text, size_t length, size_t *out_length, int *changed) {
    enum { MAX_DEPTH = 32 };
    struct { size_t start; const char *id; size_t id_len; } open[MAX_DEPTH];
    int depth = 0;
    catalog_scan scan = { 0 };

    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '"') {
            size_t s = i + 1, e = s;
            while (e < length && text[e] != '"') e += (text[e] == '\\') ? 2 : 1;
            if (depth > 0 && depth <= MAX_DEPTH && e - s == 3 && memcmp(text + s, "_id", 3) == 0 &&
                e + 2 < length && text[e + 1] == ':' && text[e + 2] == '"') {
                size_t vs = e + 3, ve = vs;
                while (ve < length && text[ve] != '"') ve += (text[ve] == '\\') ? 2 : 1;
                open[depth - 1].id = text + vs;
                open[depth - 1].id_len = ve - vs;
                e = ve;
            }
            i = e;
        } else if (c == '{') {
            if (depth < MAX_DEPTH) {
                open[depth].start = i;
                open[depth].id = NULL;
            }
            depth++;
        } else if (c == '}') {
            if (depth == 0) break;
            depth--;
            if (depth < MAX_DEPTH && open[depth].id)
                scan_item(&scan, text, open[depth].start, i + 1, open[depth].id, open[depth].id_len);
        }
    }

    *changed = scan.vehicles + scan.vinyls;
    if (!*changed) {
        free(scan.edits);
        return NULL;
    }
    char *out = (char *)malloc(length + scan.count * 64 + 1);
    if (!out) {
        free(scan.edits);
        return NULL;
    }
    size_t o = 0, from = 0;
    for (size_t k = 0; k < scan.count; k++) {
        const catalog_edit *e = &scan.edits[k];
        memcpy(out + o, text + from, e->at - from);
        o += e->at - from;
        // The paint screen shows a single price, the tokens one when there is
        // one, so vinyls are sold for credits only; the garage offers both.
        if (e->vinyl)
            o += (size_t)sprintf(out + o, "\"price\":[{\"currency\":\"credits\",\"price\":%lu}]",
                                 e->tokens * (unsigned long)s_rate);
        else
            o += (size_t)sprintf(out + o,
                                 "\"price\":[{\"currency\":\"credits\",\"price\":%lu},"
                                 "{\"currency\":\"hardcurrency\",\"price\":%lu}]",
                                 e->tokens * (unsigned long)s_rate, e->tokens);
        from = e->end;
    }
    memcpy(out + o, text + from, length - from);
    o += length - from;
    out[o] = 0;
    free(scan.edits);
    l_info("[shop] store catalog: %d token-only vehicles also priced in credits, %d vinyl groups sold for credits "
           "(%d per token)", scan.vehicles, scan.vinyls, s_rate);
    *out_length = o;
    return out;
}

static bool is_catalog(const char *begin, size_t length) {
    return length > 4096 && memmem(begin, length, "\"billing_methods\"", 17) &&
           memmem(begin, length, "___CAR_PRICE\"", 13);
}

static bool parse_hooked(int which, void *reader, const char *begin, const char *end, void *root, bool collect) {
    size_t length = (begin && end > begin) ? (size_t)(end - begin) : 0;
    if (length && is_catalog(begin, length)) {
        uint32_t crc = (uint32_t)crc32(0, (const uint8_t *)begin, (uInt)length);
        mutexLock(&s_catalog_lock);
        if (!s_rewritten || crc != s_catalog_crc || length != s_catalog_length) {
            int changed = 0;
            size_t new_length = 0;
            char *text = convert_catalog(begin, length, &new_length, &changed);
            if (text) {
                // The previous rewrite stays allocated: the reader may still
                // point into it for error messages.
                s_rewritten = text;
                s_rewritten_length = new_length;
                s_catalog_crc = crc;
                s_catalog_length = length;
            }
        }
        const char *text = (s_rewritten && crc == s_catalog_crc && length == s_catalog_length) ? s_rewritten : NULL;
        size_t text_length = s_rewritten_length;
        mutexUnlock(&s_catalog_lock);
        if (text) return s_parse_original[which](reader, text, text + text_length, root, collect);
    }
    return s_parse_original[which](reader, begin, end, root, collect);
}

static bool parse_hook_0(void *r, const char *b, const char *e, void *v, bool c) { return parse_hooked(0, r, b, e, v, c); }
static bool parse_hook_1(void *r, const char *b, const char *e, void *v, bool c) { return parse_hooked(1, r, b, e, v, c); }
static bool parse_hook_2(void *r, const char *b, const char *e, void *v, bool c) { return parse_hooked(2, r, b, e, v, c); }

static void sell_blueprint_vehicles(struct so_module *mod) {
    uint32_t *words = (uint32_t *)so_rw_ptr(mod, mod->base_addr + A8_400L_IS_CRAFTABLE);
    if (words[0] != A8_400L_IS_CRAFTABLE_WORD0 || words[1] != A8_400L_IS_CRAFTABLE_WORD1) {
        l_warn("[shop] blueprint check not where expected; blueprint vehicles stay unavailable");
        return;
    }
    words[0] = A64_MOV_W0_0;
    words[1] = A64_RET;
    l_info("[shop] blueprint vehicles sold like the others");
}

void shopmod_install(struct so_module *mod) {
    if (!configured_on()) return;
    sell_blueprint_vehicles(mod);
    static const json_parse_fn hooks[A8_400L_JSON_PARSE_COUNT] = { parse_hook_0, parse_hook_1, parse_hook_2 };

    uint32_t *spare = (uint32_t *)so_rw_ptr(mod, mod->base_addr + A8_400L_SPARE_CODE);
    bool ok = spare[0] == A8_400L_SPARE_CODE_WORD0;
    for (int i = 0; i < A8_400L_JSON_PARSE_COUNT && ok; i++) {
        const uint32_t *words = (const uint32_t *)so_rw_ptr(mod, mod->base_addr + s_parse_offsets[i]);
        ok = memcmp(words, s_parse_preimage, sizeof(s_parse_preimage)) == 0;
    }
    if (!ok) {
        l_warn("[shop] JSON reader not where expected; the store catalog keeps its prices");
        return;
    }

    for (int i = 0; i < A8_400L_JSON_PARSE_COUNT; i++) {
        uintptr_t target = mod->base_addr + s_parse_offsets[i];
        uint32_t *t = spare + i * TRAMPOLINE_WORDS;
        uintptr_t back = target + 16;
        memcpy(t, s_parse_preimage, sizeof(s_parse_preimage)); // the four instructions the hook replaces
        t[4] = 0x58000050u;                                     // ldr x16, #8
        t[5] = 0xd61f0200u;                                     // br x16
        t[6] = (uint32_t)(back & 0xFFFFFFFFu);
        t[7] = (uint32_t)(back >> 32);
        s_parse_original[i] = (json_parse_fn)(mod->base_addr + A8_400L_SPARE_CODE + (uintptr_t)i * TRAMPOLINE_WORDS * 4);
        hook_addr(target, (uintptr_t)hooks[i]);
    }
    l_info("[shop] store catalog hook installed (%d credits per token)", s_rate);
}
