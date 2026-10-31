/*
 * Lantest OS - minimal ELF64 definitions and user ELF loader
 */
#pragma once

#include <types.h>

#define ELF_PT_LOAD   1u
#define ELF_EM_X86_64 62u
#define ELF_ET_EXEC   2u
#define ELF_ET_DYN    3u

typedef struct __attribute__((packed)) {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} Elf64_Ehdr;

typedef struct __attribute__((packed)) {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
} Elf64_Phdr;

/*
 * Load an ELF64 user image into the given PML4.
 *
 * Segments are mapped page-by-page (so two PT_LOAD segments sharing a page
 * do not overwrite each other) and the loader runs in three passes:
 *   1. validate headers + allocate/map every page covered by PT_LOAD
 *   2. zero every .bss range (p_memsz > p_filesz)
 *   3. copy every file-backed range (p_filesz)
 * The copy pass runs last so file data always wins inside a shared page.
 *
 * Returns 0 on success and stores e_entry in *entry_out; -1 on any error.
 * On error the caller should paging_destroy_user_space() the PML4.
 */
int elf_load_user(u64 pml4_phys, const u8 *data, u64 size, u64 *entry_out);

struct task;
struct file;

/* Lazy exec: validate PT_LOADs, add file-backed VMAs, map nothing. */
int elf_load_vmas(struct task *t, struct file *file,
                  const u8 *data, u64 size, u64 *entry_out);
