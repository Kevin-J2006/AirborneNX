#include "so_util.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#define ALIGN_PAGE(x) (((x) + 0xFFF) & ~0xFFF)

static so_module *s_mod_head = NULL;

static int symbol_matches(const char *elf_sym, const char *target_sym) {
    if (strcmp(elf_sym, target_sym) == 0)
        return 1;

    // Check if elf_sym has @ (e.g. foo@LIBC or foo@@LIBC)
    const char *at = strchr(elf_sym, '@');
    if (at) {
        size_t len = at - elf_sym;
        if (strlen(target_sym) == len && strncmp(elf_sym, target_sym, len) == 0)
            return 1;
    }

    // Check if target_sym has @
    at = strchr(target_sym, '@');
    if (at) {
        size_t len = at - target_sym;
        if (strlen(elf_sym) == len && strncmp(elf_sym, target_sym, len) == 0)
            return 1;
    }

    return 0;
}

so_module *so_module_list(void) {
    return s_mod_head;
}

void so_unresolved_handler(uintptr_t got_addr) {
    for (so_module *m = s_mod_head; m != NULL; m = m->next) {
        if (m->plt_rela) {
            for (uint32_t i = 0; i < m->num_plt_rela; i++) {
                uintptr_t target = m->base_addr + m->plt_rela[i].r_offset;
                if (target == got_addr) { // the GOT slot's run-time address
                    uint32_t sym_idx = ELF64_R_SYM(m->plt_rela[i].r_info);
                    const char *name = m->dynstr + m->dynsym[sym_idx].st_name;
                    fatal_error("[so_util][%s] CRASH: Called unresolved function '%s' (GOT: 0x%lx)",
                                m->soname ? m->soname : "mod", name, (unsigned long)got_addr);
                }
            }
        }
        if (m->rela) {
            for (uint32_t i = 0; i < m->num_rela; i++) {
                uintptr_t target = m->base_addr + m->rela[i].r_offset;
                if (target == got_addr) {
                    uint32_t sym_idx = ELF64_R_SYM(m->rela[i].r_info);
                    const char *name = m->dynstr + m->dynsym[sym_idx].st_name;
                    fatal_error("[so_util][%s] CRASH: Called unresolved symbol '%s' (GOT: 0x%lx)",
                                m->soname ? m->soname : "mod", name, (unsigned long)got_addr);
                }
            }
        }
    }
    fatal_error("[so_util] CRASH: Called unresolved function at GOT 0x%lx", (unsigned long)got_addr);
}

extern void so_unresolved_stub(void);

int so_load(so_module *mod, const char *filename) {
    memset(mod, 0, sizeof(so_module));

    FILE *f = fopen(filename, "rb");
    if (!f) {
        l_error("[so_util] Error: Cannot open file '%s'", filename);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    mod->ehdr = (Elf64_Ehdr *)malloc(sizeof(Elf64_Ehdr));
    if (fread(mod->ehdr, 1, sizeof(Elf64_Ehdr), f) != sizeof(Elf64_Ehdr)) {
        l_error("[so_util] Error: Failed to read ELF header of '%s'", filename);
        fclose(f);
        return -2;
    }

    if (memcmp(mod->ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        mod->ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        mod->ehdr->e_machine != EM_AARCH64) {
        l_error("[so_util] Error: '%s' is not a valid 64-bit AArch64 ELF binary", filename);
        fclose(f);
        return -3;
    }

    size_t phdr_size = mod->ehdr->e_phnum * sizeof(Elf64_Phdr);
    mod->phdr = (Elf64_Phdr *)malloc(phdr_size);
    fseek(f, mod->ehdr->e_phoff, SEEK_SET);
    if (fread(mod->phdr, 1, phdr_size, f) != phdr_size) {
        l_error("[so_util] Error: Failed to read program headers");
        fclose(f);
        return -4;
    }

    uintptr_t min_vaddr = (uintptr_t)-1;
    uintptr_t max_vaddr = 0;

    for (int i = 0; i < mod->ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &mod->phdr[i];
        if (ph->p_type == PT_LOAD) {
            if (ph->p_vaddr < min_vaddr)
                min_vaddr = ph->p_vaddr;
            if (ph->p_vaddr + ph->p_memsz > max_vaddr)
                max_vaddr = ph->p_vaddr + ph->p_memsz;
        }
    }

    mod->total_size = ALIGN_PAGE(max_vaddr - min_vaddr);

    // Horizon only executes memory mapped as code. The image is built in heap
    // memory, linked as if it already sat in a reserved slice of the code
    // region, and moved there by so_initialize() once nothing has to write
    // to its text any more.
    mod->load_base = (uintptr_t)memalign(0x1000, mod->total_size);
    if (!mod->load_base) {
        l_error("[so_util] Error: Failed to allocate memory (size=0x%zx)", mod->total_size);
        fclose(f);
        return -5;
    }

    virtmemLock();
    mod->base_addr = (uintptr_t)virtmemFindCodeMemory(mod->total_size, 0x1000);
    if (mod->base_addr)
        mod->reservation = virtmemAddReservation((void *)mod->base_addr, mod->total_size);
    virtmemUnlock();
    if (!mod->base_addr) {
        l_error("[so_util] Error: No code address space for '%s' (size=0x%zx)", filename, mod->total_size);
        fclose(f);
        return -5;
    }
    l_info("[so_util] Image at 0x%lx, to be mapped as code at 0x%lx, size=0x%zx",
           (unsigned long)mod->load_base, (unsigned long)mod->base_addr, mod->total_size);

    memset((void *)mod->load_base, 0, mod->total_size);

    for (int i = 0; i < mod->ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &mod->phdr[i];
        if (ph->p_type == PT_LOAD) {
            void *dst = (void *)(mod->load_base + ph->p_vaddr);
            if (ph->p_filesz > 0) {
                fseek(f, ph->p_offset, SEEK_SET);
                fread(dst, 1, ph->p_filesz, f);
            }
            if (ph->p_memsz > ph->p_filesz) {
                memset((void *)((uintptr_t)dst + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
            }

            if (ph->p_flags & PF_X) {
                mod->text_base = mod->base_addr + ph->p_vaddr;
                mod->text_size = ph->p_memsz;
            } else if (ph->p_flags & PF_W) {
                if (mod->num_data_segs < MAX_DATA_SEG) {
                    mod->data_base[mod->num_data_segs] = mod->base_addr + ph->p_vaddr;
                    mod->data_size[mod->num_data_segs] = ph->p_memsz;
                    mod->num_data_segs++;
                }
            }
        } else if (ph->p_type == PT_DYNAMIC) {
            mod->dynamic = (Elf64_Dyn *)(mod->load_base + ph->p_vaddr);
            mod->num_dynamic = ph->p_memsz / sizeof(Elf64_Dyn);
        }
    }

    fclose(f);

    if (!mod->dynamic) {
        l_error("[so_util] Error: PT_DYNAMIC segment not found in '%s'", filename);
        return -6;
    }

    uint32_t soname_offset = 0;

    for (Elf64_Dyn *dyn = mod->dynamic; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
            case DT_SYMTAB:
                mod->dynsym = (Elf64_Sym *)(mod->load_base + dyn->d_un.d_ptr);
                break;
            case DT_STRTAB:
                mod->dynstr = (char *)(mod->load_base + dyn->d_un.d_ptr);
                break;
            case DT_RELA:
                mod->rela = (Elf64_Rela *)(mod->load_base + dyn->d_un.d_ptr);
                break;
            case DT_RELASZ:
                mod->num_rela = dyn->d_un.d_val / sizeof(Elf64_Rela);
                break;
            case DT_JMPREL:
                mod->plt_rela = (Elf64_Rela *)(mod->load_base + dyn->d_un.d_ptr);
                break;
            case DT_PLTRELSZ:
                mod->num_plt_rela = dyn->d_un.d_val / sizeof(Elf64_Rela);
                break;
            case DT_INIT_ARRAY:
                mod->init_array = (void (**)(void))(mod->load_base + dyn->d_un.d_ptr);
                break;
            case DT_INIT_ARRAYSZ:
                mod->num_init_array = dyn->d_un.d_val / sizeof(void *);
                break;
            case DT_FINI_ARRAY:
                mod->fini_array = (void (**)(void))(mod->load_base + dyn->d_un.d_ptr);
                break;
            case DT_FINI_ARRAYSZ:
                mod->num_fini_array = dyn->d_un.d_val / sizeof(void *);
                break;
            case DT_HASH:
                mod->hash = (uint32_t *)(mod->load_base + dyn->d_un.d_ptr);
                mod->num_dynsym = mod->hash[1];
                break;
            case DT_SONAME:
                soname_offset = (uint32_t)dyn->d_un.d_val;
                break;
        }
    }

    if (mod->dynstr && soname_offset) {
        // Copied: dynstr moves when the module is mapped.
        mod->soname = strdup(mod->dynstr + soname_offset);
    } else {
        // Fallback to filename basename
        const char *slash = strrchr(filename, '/');
        mod->soname = (char *)(slash ? slash + 1 : filename);
    }

    if (!mod->hash && mod->dynsym && mod->dynstr) {
        // Fallback count if hash is absent
        mod->num_dynsym = ((uintptr_t)mod->dynstr - (uintptr_t)mod->dynsym) / sizeof(Elf64_Sym);
    }

    // Add to global module list
    mod->next = s_mod_head;
    s_mod_head = mod;

    l_info("[so_util] Loaded '%s' (%s): symbols=%u, rela=%u, plt_rela=%u, inits=%u",
           filename, mod->soname, mod->num_dynsym, mod->num_rela, mod->num_plt_rela, mod->num_init_array);
    return 0;
}

void *so_rw_ptr(so_module *mod, uintptr_t addr) {
    if (mod->mapped) return (void *)addr;
    return (void *)(addr - mod->base_addr + mod->load_base);
}

uintptr_t so_symbol(so_module *mod, const char *name) {
    if (!mod || !mod->dynsym || !mod->dynstr)
        return 0;

    for (uint32_t i = 0; i < mod->num_dynsym; i++) {
        Elf64_Sym *sym = &mod->dynsym[i];
        if (sym->st_shndx != SHN_UNDEF && sym->st_name != 0) {
            const char *sym_name = mod->dynstr + sym->st_name;
            if (symbol_matches(name, sym_name) || symbol_matches(sym_name, name)) {
                return mod->base_addr + sym->st_value;
            }
        }
    }
    return 0;
}

static void apply_rela_list(so_module *mod, Elf64_Rela *rela_table, uint32_t count,
                            so_default_dynlib *default_lib, int default_lib_size) {
    if (!rela_table || count == 0)
        return;

    for (uint32_t i = 0; i < count; i++) {
        Elf64_Rela *rela = &rela_table[i];
        uint32_t type = ELF64_R_TYPE(rela->r_info);
        uint32_t sym_idx = ELF64_R_SYM(rela->r_info);
        uintptr_t *target = (uintptr_t *)so_rw_ptr(mod, mod->base_addr + rela->r_offset);

        switch (type) {
            case R_AARCH64_NONE:
                break;

            case R_AARCH64_RELATIVE:
                *target = mod->base_addr + rela->r_addend;
                break;

            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT:
            case R_AARCH64_ABS64: {
                if (sym_idx >= mod->num_dynsym)
                    break;

                Elf64_Sym *sym = &mod->dynsym[sym_idx];

                if (sym->st_shndx != SHN_UNDEF) {
                    // Symbol defined internally in this module
                    *target = mod->base_addr + sym->st_value + rela->r_addend;
                } else if (sym->st_name != 0) {
                    const char *name = mod->dynstr + sym->st_name;
                    uintptr_t resolved_addr = 0;

                    // 1. Search default_lib FIRST (host platform wrappers take precedence)
                    if (default_lib) {
                        for (int j = 0; j < default_lib_size; j++) {
                            if (symbol_matches(name, default_lib[j].symbol)) {
                                resolved_addr = default_lib[j].func;
                                break;
                            }
                        }
                    }

                    // 2. Search other loaded modules (e.g. libc++_shared.so exports)
                    if (!resolved_addr) {
                        for (so_module *curr = s_mod_head; curr != NULL; curr = curr->next) {
                            if (curr != mod) {
                                resolved_addr = so_symbol(curr, name);
                                if (resolved_addr)
                                    break;
                            }
                        }
                    }

                    // 3. Apply resolved address or fallback
                    if (resolved_addr) {
                        *target = resolved_addr + rela->r_addend;
                    } else {
                        if (type == R_AARCH64_JUMP_SLOT) {
                            *target = (uintptr_t)&so_unresolved_stub;
                            l_warn("[so_util][%s] Unresolved PLT symbol: %s",
                                   mod->soname ? mod->soname : "mod", name);
                        } else {
                            l_warn("[so_util][%s] Unresolved DATA symbol: %s",
                                   mod->soname ? mod->soname : "mod", name);
                        }
                    }
                }
                break;
            }

            default:
                l_warn("[so_util][%s] Unsupported relocation type %u at offset 0x%lx",
                       mod->soname ? mod->soname : "mod", type, (unsigned long)rela->r_offset);
                break;
        }
    }
}

int so_relocate(so_module *mod) {
    if (mod->rela && mod->num_rela > 0)
        apply_rela_list(mod, mod->rela, mod->num_rela, NULL, 0);
    if (mod->plt_rela && mod->num_plt_rela > 0)
        apply_rela_list(mod, mod->plt_rela, mod->num_plt_rela, NULL, 0);
    return 0;
}

int so_resolve(so_module *mod, so_default_dynlib *default_lib, int size, int check_missing) {
    (void)check_missing;
    if (mod->rela && mod->num_rela > 0)
        apply_rela_list(mod, mod->rela, mod->num_rela, default_lib, size);
    if (mod->plt_rela && mod->num_plt_rela > 0)
        apply_rela_list(mod, mod->plt_rela, mod->num_plt_rela, default_lib, size);
    return 0;
}

// Moves the finished image from the heap into the code region: text becomes
// read+execute, everything else read+write.
static void map_as_code(so_module *mod) {
    const char *name = mod->soname ? mod->soname : "mod";
    // hbloader passes a real handle to the process; without a loader (an
    // emulator booting the NRO directly) only the pseudo-handle exists.
    Handle process = envGetOwnProcessHandle();
    if (process == INVALID_HANDLE) process = CUR_PROCESS_HANDLE;
    uintptr_t text_start = mod->text_base & ~(uintptr_t)0xFFF;
    uintptr_t text_end = ALIGN_PAGE(mod->text_base + mod->text_size);
    uintptr_t end = mod->base_addr + mod->total_size;

    for (int i = 0; i < mod->num_data_segs; i++) {
        if (mod->data_base[i] < text_end && mod->data_base[i] + mod->data_size[i] > text_start)
            fatal_error("[so_util][%s] Text and data share a page; cannot be mapped as code", name);
    }

    armDCacheFlush((void *)mod->load_base, mod->total_size);

    Result rc = svcMapProcessCodeMemory(process, mod->base_addr, mod->load_base, mod->total_size);
    if (R_FAILED(rc))
        fatal_error("[so_util][%s] svcMapProcessCodeMemory failed: 0x%x", name, rc);

    struct { uintptr_t start, end; u32 perm; } ranges[] = {
        { mod->base_addr, text_start, Perm_Rw },
        { text_start, text_end, Perm_Rx },
        { text_end, end, Perm_Rw },
    };
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        if (ranges[i].end <= ranges[i].start) continue;
        rc = svcSetProcessMemoryPermission(process, ranges[i].start, ranges[i].end - ranges[i].start,
                                           ranges[i].perm);
        if (R_FAILED(rc))
            fatal_error("[so_util][%s] svcSetProcessMemoryPermission(0x%lx, perm %u) failed: 0x%x",
                        name, (unsigned long)ranges[i].start, ranges[i].perm, rc);
    }

    // The heap pages are gone from under load_base; follow the image.
    intptr_t delta = (intptr_t)mod->base_addr - (intptr_t)mod->load_base;
#define REBASE(field) if (mod->field) mod->field = (void *)((uintptr_t)mod->field + delta)
    REBASE(dynamic);
    REBASE(dynsym);
    REBASE(dynstr);
    REBASE(rela);
    REBASE(plt_rela);
    REBASE(init_array);
    REBASE(fini_array);
    REBASE(hash);
#undef REBASE
    mod->mapped = true;

    armICacheInvalidate((void *)text_start, text_end - text_start);
    l_info("[so_util][%s] Mapped as code at 0x%lx (text 0x%lx-0x%lx)", name,
           (unsigned long)mod->base_addr, (unsigned long)text_start, (unsigned long)text_end);
}

int so_initialize(so_module *mod) {
    map_as_code(mod);

    extern void pthr_init_main(void);
    pthr_init_main();

    l_info("[so_util][%s] Executing %u DT_INIT_ARRAY constructors...",
           mod->soname ? mod->soname : "mod", mod->num_init_array);

    if (mod->init_array && mod->num_init_array > 0) {
        for (uint32_t i = 0; i < mod->num_init_array; i++) {
            if (mod->init_array[i]) {
                mod->init_array[i]();
            }
        }
    }

    l_info("[so_util][%s] Initialization completed successfully",
           mod->soname ? mod->soname : "mod");
    return 0;
}

so_hook hook_addr(uintptr_t addr, uintptr_t dst) {
    so_hook hook;
    hook.addr = addr;

    // Text is only writable while its module is still a heap image.
    uint32_t *target = NULL;
    for (so_module *m = s_mod_head; m != NULL; m = m->next) {
        if (addr < m->base_addr || addr >= m->base_addr + m->total_size) continue;
        if (m->mapped)
            fatal_error("[so_util] hook_addr(0x%lx) after the module was mapped as code", (unsigned long)addr);
        target = (uint32_t *)so_rw_ptr(m, addr);
    }
    if (!target)
        fatal_error("[so_util] hook_addr(0x%lx): address is not in a loaded module", (unsigned long)addr);

    for (int i = 0; i < 4; i++) {
        hook.orig_instr[i] = target[i];
    }

    // AArch64 absolute 64-bit jump:
    // LDR X16, #8   (0x58000050)
    // BR  X16       (0xD61F0200)
    // [64-bit dst address, 8 bytes]
    hook.patch_instr[0] = 0x58000050; // ldr x16, #8
    hook.patch_instr[1] = 0xD61F0200; // br x16
    hook.patch_instr[2] = (uint32_t)(dst & 0xFFFFFFFF);
    hook.patch_instr[3] = (uint32_t)((dst >> 32) & 0xFFFFFFFF);

    for (int i = 0; i < 4; i++) {
        target[i] = hook.patch_instr[i];
    }

    return hook;
}
