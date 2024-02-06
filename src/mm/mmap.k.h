#ifndef _MM__MMAP_K_H
#define _MM__MMAP_K_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/vector.k.h>
#include <mm/vmm.k.h>
#include <sys/cpu.k.h>
#include <sys/mman.h>
#include <sys/types.h>

struct mmap_range_global {
    struct pagemap *shadow_pagemap;
    VECTOR_TYPE(struct mmap_range_local *) locals;
    struct resource *res;
    uintptr_t base;
    size_t length;
    off_t offset;
};

struct mmap_range_local {
    struct pagemap *pagemap;
    struct mmap_range_global *global;
    uintptr_t base;
    size_t length;
    off_t offset;
    int prot;
    int flags;
};

void mmap_list_ranges(struct pagemap *pagemap);
bool mmap_handle_pf(struct cpu_ctx *ctx);
bool mmap_page_in_range(struct mmap_range_global *global, uintptr_t virt,
                            uintptr_t phys, int prot);
bool mmap_range(struct pagemap *pagemap, uintptr_t virt, uintptr_t phys,
                size_t length, int prot, int flags);
void *mmap(struct pagemap *pagemap, uintptr_t addr, size_t length, int prot,
           int flags, struct resource *res, off_t offset);
bool munmap(struct pagemap *pagemap, uintptr_t addr, size_t length);

#endif
