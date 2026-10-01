#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <switch.h>
#include <elf.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_DATA_SEG 4

/**
 * @brief Runtime state of a loaded ARM64 Android shared library (.so).
 */
typedef struct so_module {
    struct so_module *next;

    // Address the module runs at. Until so_initialize() maps it there, its
    // image lives in ordinary heap memory at load_base and can only be read
    // or written through so_rw_ptr().
    uintptr_t base_addr;
    uintptr_t load_base;
    size_t total_size;
    bool mapped;
    VirtmemReservation *reservation;

    uintptr_t text_base;
    size_t text_size;

    uintptr_t data_base[MAX_DATA_SEG];
    size_t data_size[MAX_DATA_SEG];
    int num_data_segs;

    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    Elf64_Shdr *shdr;

    Elf64_Dyn *dynamic;
    Elf64_Sym *dynsym;
    Elf64_Rela *rela;
    Elf64_Rela *plt_rela;

    void (**init_array)(void);
    void (**fini_array)(void);
    uint32_t *hash;

    uint32_t num_dynamic;
    uint32_t num_dynsym;
    uint32_t num_rela;
    uint32_t num_plt_rela;
    uint32_t num_init_array;
    uint32_t num_fini_array;

    char *soname;
    char *dynstr;
} so_module;

/**
 * @brief Saved state for an AArch64 function hook.
 */
typedef struct {
    uintptr_t addr;
    uint32_t orig_instr[4];
    uint32_t patch_instr[4];
} so_hook;

/**
 * @brief Maps an unresolved dynamic symbol name to a function/variable pointer.
 */
typedef struct {
    const char *symbol;
    uintptr_t func;
} so_default_dynlib;

/**
 * @brief Head of the linked list of loaded modules (most recent first).
 */
so_module *so_module_list(void);

/**
 * @brief Load an ARM64 ELF shared object from disk.
 *
 * @param mod       Pointer to so_module struct to populate.
 * @param filename  Path to .so file.
 * @return 0 on success, negative error code on failure.
 */
int so_load(so_module *mod, const char *filename);

/**
 * @brief Resolve dynamic symbols using the provided symbol table.
 *
 * @param mod           Target so_module.
 * @param default_lib   Array of so_default_dynlib entries.
 * @param size          Number of entries in array.
 * @param check_missing If non-zero, logs missing symbols.
 * @return 0 on success.
 */
int so_resolve(so_module *mod, so_default_dynlib *default_lib, int size, int check_missing);

/**
 * @brief Apply AArch64 relocations (R_AARCH64_RELATIVE, GLOB_DAT, JUMP_SLOT, ABS64).
 *
 * @param mod Target so_module.
 * @return 0 on success.
 */
int so_relocate(so_module *mod);

/**
 * @brief Map the module as code at base_addr and run DT_INIT_ARRAY constructors.
 *
 * Text becomes read+execute and can no longer be patched after this call.
 *
 * @param mod Target so_module.
 * @return 0 on success.
 */
int so_initialize(so_module *mod);

/**
 * @brief Query symbol address by name from the module's dynamic symbol table.
 *
 * @param mod  Target so_module.
 * @param name Symbol name.
 * @return Address of symbol, or 0 if not found.
 */
uintptr_t so_symbol(so_module *mod, const char *name);

/**
 * @brief Pointer through which a module address can be read or written now.
 *
 * @param mod  Target so_module.
 * @param addr Address inside the module, as so_symbol() or base_addr give it.
 * @return The heap image of that address before so_initialize(), addr after.
 */
void *so_rw_ptr(so_module *mod, uintptr_t addr);

/**
 * @brief Install an absolute 64-bit branch hook at a target function address.
 *
 * Must be called before so_initialize() on the module that owns addr.
 *
 * @param addr Destination function to hook.
 * @param dst  Hook function to redirect to.
 * @return so_hook handle.
 */
so_hook hook_addr(uintptr_t addr, uintptr_t dst);

#endif // __SO_UTIL_H__
