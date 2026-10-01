#include "patch.h"
#include "utils/init.h"
#include "utils/logger.h"
#include <stdio.h>

/* Exact Asphalt 8 4.0.0l arm64-v8a offsets and instruction preimages.  The
 * boot state selector (0xD30B5C) calls the profile's user-age getter and
 * creates GS_AgeGating when it returns -1 (never set).  That screen cannot be
 * completed here, so report a fixed adult age before state selection, the
 * same way the Vita port does for 2.0.0e.  The getter is only two
 * instructions long and the next function starts right after it, so the
 * 16-byte hook_addr trampoline does not fit: rewrite its load in place. */
#define A8_400L_USER_AGE_GET_OFFSET 0x1372EBCu
#define A8_400L_USER_AGE_GET_WORD0 0xB94BC800u /* ldr w0, [x0, #0xbc8] */
#define A8_400L_USER_AGE_GET_WORD1 0xD65F03C0u /* ret */
#define A8_400L_AGE_GATE_SELECT_OFFSET 0xD30B60u
#define A8_400L_AGE_GATE_SELECT_WORD0 0x941908D7u /* bl user_age_get */
#define A8_400L_AGE_GATE_SELECT_WORD1 0x3100041Fu /* cmn w0, #1 */
#define A8_AGE_VALUE 33
#define A64_MOVZ_W0(imm16) (0x52800000u | ((uint32_t)(imm16) << 5))

static int dummy_native_allow(void *env, void *clazz) {
    (void)env;
    (void)clazz;
    return 1; // Always allow DRM policy
}

static void install_age_gate_bypass(so_module *mod) {
    uint32_t *getter_words =
        (uint32_t *)so_rw_ptr(mod, mod->base_addr + A8_400L_USER_AGE_GET_OFFSET);
    const uint32_t *selector_words =
        (const uint32_t *)so_rw_ptr(mod, mod->base_addr + A8_400L_AGE_GATE_SELECT_OFFSET);

    if (getter_words[0] != A8_400L_USER_AGE_GET_WORD0 ||
        getter_words[1] != A8_400L_USER_AGE_GET_WORD1 ||
        selector_words[0] != A8_400L_AGE_GATE_SELECT_WORD0 ||
        selector_words[1] != A8_400L_AGE_GATE_SELECT_WORD1) {
        fatal_error("Asphalt 8 age-gate patch preimage mismatch.");
    }

    getter_words[0] = A64_MOVZ_W0(A8_AGE_VALUE);
    l_info("User age pinned to %d before state selection", A8_AGE_VALUE);
}

/* Every profile in GameOptions.json sets the FPSLimit option to 30.  The
 * options loader converts it to an integer at 0x45536C and stores it in the
 * options block; the frame period (1000000 / FPSLimit microseconds), the
 * simulation step and the settings screen are all derived from that field.
 * Replace the conversion with the wanted rate so that all of them agree. */
#define A8_400L_FPS_LIMIT_OFFSET 0x45536Cu
#define A8_400L_FPS_LIMIT_WORD0 0x1E38010Au /* fcvtzs w10, s8 */
#define A8_400L_FPS_LIMIT_WORD1 0xB902B7E8u /* str w8, [sp, #0x2b4] */
#define A64_MOVZ_W10(imm16) (0x5280000Au | ((uint32_t)(imm16) << 5))
#define A8_DEFAULT_FPS 60

// "fps=30" or "fps=60" in config.ini picks the frame rate without a rebuild.
static int configured_fps(void) {
    int fps = A8_DEFAULT_FPS;
    FILE *f = fopen(DATA_PATH "config.ini", "r");
    if (!f) return fps;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int value = 0;
        if (sscanf(line, " fps = %d", &value) == 1 && (value == 30 || value == 60)) fps = value;
    }
    fclose(f);
    return fps;
}

static void install_fps_limit(so_module *mod) {
    int fps = configured_fps();
    uint32_t *words = (uint32_t *)so_rw_ptr(mod, mod->base_addr + A8_400L_FPS_LIMIT_OFFSET);
    if (words[0] != A8_400L_FPS_LIMIT_WORD0 || words[1] != A8_400L_FPS_LIMIT_WORD1)
        fatal_error("Asphalt 8 frame-limit patch preimage mismatch.");

    words[0] = A64_MOVZ_W10(fps);
    l_info("Frame limit set to %d FPS", fps);
}

void so_patch(so_module *mod) {
    l_info("Applying AArch64 binary patches...");

    uintptr_t gdrm_allow = so_symbol(mod, "Java_com_gameloft_android_ANMP_GloftA8HM_installer_GDRMPolicy_nativeAllow");
    if (gdrm_allow) {
        hook_addr(gdrm_allow, (uintptr_t)dummy_native_allow);
        l_info("Hooked GDRMPolicy_nativeAllow -> allowed");
    }

    install_age_gate_bypass(mod);
    install_fps_limit(mod);

    l_info("All runtime patches applied successfully.");
}
