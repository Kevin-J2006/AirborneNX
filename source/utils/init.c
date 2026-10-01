#include "init.h"
#include "logger.h"
#include "../reimpl/pthr.h"
#include <switch.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

static int make_dir_p(const char *dir) {
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return mkdir(tmp, 0777);
}

static void write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

// The engine picks its CPU/GPU/memory quality profiles (GameOptions.json) from
// /proc and /sys. Without them it reads 0 MB of RAM and falls back to the
// lowest texture settings, so describe the Switch (4x Cortex-A57, 4 GB) the
// way an Android kernel would. translate_path() maps /proc and /sys here.
static void write_sysroot(void) {
    static const char cpu_block[] =
        "processor\t: %d\n"
        "BogoMIPS\t: 38.40\n"
        "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
        "CPU implementer\t: 0x41\n"
        "CPU architecture: 8\n"
        "CPU variant\t: 0x1\n"
        "CPU part\t: 0xd07\n"
        "CPU revision\t: 1\n\n";
    char text[2048];
    size_t len = 0;
    for (int cpu = 0; cpu < 4; cpu++)
        len += snprintf(text + len, sizeof(text) - len, cpu_block, cpu);
    snprintf(text + len, sizeof(text) - len, "Hardware\t: Nintendo Switch\n");

    make_dir_p(SYSROOT_PATH "proc/");
    write_text_file(SYSROOT_PATH "proc/cpuinfo", text);
    write_text_file(SYSROOT_PATH "proc/meminfo",
                    "MemTotal:        3891200 kB\n"
                    "MemFree:         2621440 kB\n"
                    "MemAvailable:    2883584 kB\n"
                    "Buffers:               0 kB\n"
                    "Cached:           262144 kB\n"
                    "SwapTotal:             0 kB\n"
                    "SwapFree:              0 kB\n");

    for (int cpu = 0; cpu < 4; cpu++) {
        char dir[160], path[200];
        snprintf(dir, sizeof(dir), SYSROOT_PATH "sys/devices/system/cpu/cpu%d/cpufreq/", cpu);
        make_dir_p(dir);
        snprintf(path, sizeof(path), "%scpuinfo_max_freq", dir);
        write_text_file(path, "1785000\n");
        snprintf(path, sizeof(path), "%scpuinfo_min_freq", dir);
        write_text_file(path, "204000\n");
        snprintf(path, sizeof(path), "%sscaling_cur_freq", dir);
        write_text_file(path, "1785000\n");
    }
}

int ensure_directories(void) {
    make_dir_p(DATA_PATH);
    make_dir_p(FILES_PATH);
    make_dir_p(INTERNAL_PATH);
    make_dir_p(CACHE_PATH);
    make_dir_p(ASSETS_PATH);
    make_dir_p(DATA_PATH "filesupdate/");
    make_dir_p(DATA_PATH "sdcard/");
    write_sysroot();
    return 0;
}

int init_nx(void) {
    pthr_init_main();
    ensure_directories();
    log_init();
    l_info("Initializing Switch hardware services...");
    l_info("Configured Bionic TLS (TPIDR_EL0) for Android 64-bit compatibility");
    romfsInit();
    return 0;
}

void cleanup_nx(void) {
    l_info("Cleaning up Switch hardware services...");
    romfsExit();
}
