#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <lib/alloc.k.h>
#include <lib/errno.k.h>
#include <lib/lock.k.h>
#include <lib/misc.k.h>
#include <lib/panic.k.h>
#include <lib/resource.k.h>
#include <lib/vector.k.h>
#include <mm/pmm.k.h>
#include <mm/mmap.k.h>
#include <mm/vmm.k.h>

volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

static volatile struct limine_kernel_address_request kaddr_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

struct pagemap *vmm_kernel_pagemap = NULL;
bool vmm_initialised = false;

extern symbol text_start_addr, text_end_addr;
extern symbol rodata_start_addr, rodata_end_addr;
extern symbol data_start_addr, data_end_addr;

static uint64_t *get_next_level(uint64_t *top_level, size_t idx, bool allocate) {
    if ((top_level[idx] & PTE_PRESENT) != 0) {
        return (uint64_t *)(PTE_GET_ADDR(top_level[idx]) + VMM_HIGHER_HALF);
    }

    if (!allocate) {
        return NULL;
    }

    void *next_level = pmm_alloc(1);
    if (next_level == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    top_level[idx] = (uint64_t)next_level | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    return next_level + VMM_HIGHER_HALF;
}

void vmm_init(void) {
    ASSERT(kaddr_request.response != NULL);

    vmm_kernel_pagemap = ALLOC(struct pagemap);
    vmm_kernel_pagemap->lock = (spinlock_t)SPINLOCK_INIT;
    vmm_kernel_pagemap->top_level = pmm_alloc(1);

    ASSERT(vmm_kernel_pagemap->top_level != NULL);
    vmm_kernel_pagemap->top_level = (void *)vmm_kernel_pagemap->top_level + VMM_HIGHER_HALF;

    for (size_t i = 256; i < 512; i++) {
        ASSERT(get_next_level(vmm_kernel_pagemap->top_level, i, true) != NULL);
    }

    uintptr_t text_start = ALIGN_DOWN((uintptr_t)text_start_addr, PAGE_SIZE),
        rodata_start = ALIGN_DOWN((uintptr_t)rodata_start_addr, PAGE_SIZE),
        data_start = ALIGN_DOWN((uintptr_t)data_start_addr, PAGE_SIZE),
        text_end = ALIGN_UP((uintptr_t)text_end_addr, PAGE_SIZE),
        rodata_end = ALIGN_UP((uintptr_t)rodata_end_addr, PAGE_SIZE),
        data_end = ALIGN_UP((uintptr_t)data_end_addr, PAGE_SIZE);

    struct limine_kernel_address_response *kaddr = kaddr_request.response;

    for (uintptr_t text_addr = text_start; text_addr < text_end; text_addr += PAGE_SIZE) {
        uintptr_t phys = text_addr - kaddr->virtual_base + kaddr->physical_base;
        ASSERT(vmm_map_page(vmm_kernel_pagemap, text_addr, phys, PTE_PRESENT));
    }

    for (uintptr_t rodata_addr = rodata_start; rodata_addr < rodata_end; rodata_addr += PAGE_SIZE) {
        uintptr_t phys = rodata_addr - kaddr->virtual_base + kaddr->physical_base;
        ASSERT(vmm_map_page(vmm_kernel_pagemap, rodata_addr, phys, PTE_PRESENT | PTE_NX));
    }

    for (uintptr_t data_addr = data_start; data_addr < data_end; data_addr += PAGE_SIZE) {
        uintptr_t phys = data_addr - kaddr->virtual_base + kaddr->physical_base;
        ASSERT(vmm_map_page(vmm_kernel_pagemap, data_addr, phys, PTE_PRESENT | PTE_WRITABLE | PTE_NX));
    }

    for (uintptr_t addr = 0x1000; addr < 0x100000000; addr += PAGE_SIZE) {
        ASSERT(vmm_map_page(vmm_kernel_pagemap, addr, addr, PTE_PRESENT | PTE_WRITABLE));
        ASSERT(vmm_map_page(vmm_kernel_pagemap, addr + VMM_HIGHER_HALF, addr, PTE_PRESENT | PTE_WRITABLE | PTE_NX));
    }

    struct limine_memmap_response *memmap = memmap_request.response;
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        uintptr_t base = ALIGN_DOWN(entry->base, PAGE_SIZE);
        uintptr_t top = ALIGN_UP(entry->base + entry->length, PAGE_SIZE);
        if (top <= 0x100000000) {
            continue;
        }

        for (uintptr_t j = base; j < top; j += PAGE_SIZE) {
            if (j < 0x100000000) {
                continue;
            }

            ASSERT(vmm_map_page(vmm_kernel_pagemap, j, j, PTE_PRESENT | PTE_WRITABLE));
            ASSERT(vmm_map_page(vmm_kernel_pagemap, j + VMM_HIGHER_HALF, j, PTE_PRESENT | PTE_WRITABLE | PTE_NX));
        }
    }

    vmm_switch_to(vmm_kernel_pagemap);
    vmm_initialised = true;
}

struct pagemap *vmm_new_pagemap(void) {
    struct pagemap *pagemap = ALLOC(struct pagemap);
    if (pagemap == NULL) {
        errno = ENOMEM;
        goto cleanup;
    }

    pagemap->lock = (spinlock_t)SPINLOCK_INIT;
    pagemap->top_level = pmm_alloc(1);
    if (pagemap->top_level == NULL) {
        errno = ENOMEM;
        goto cleanup;
    }

    pagemap->top_level = (void *)pagemap->top_level + VMM_HIGHER_HALF;
    for (size_t i = 256; i < 512; i++) {
        pagemap->top_level[i] = vmm_kernel_pagemap->top_level[i];
    }

    return pagemap;

cleanup:
    if (pagemap != NULL) {
        free(pagemap);
    }

    return NULL;
}

struct pagemap *vmm_fork_pagemap(struct pagemap *pagemap) {
    spinlock_acquire(&pagemap->lock);

    struct pagemap *new_pagemap = vmm_new_pagemap();
    if (new_pagemap == NULL) {
        goto cleanup;
    }

    VECTOR_FOR_EACH(&pagemap->mmap_ranges, it,
        struct mmap_range_local *local_range = *it;
        struct mmap_range_global *global_range = local_range->global;

        struct mmap_range_local *new_local_range = ALLOC(struct mmap_range_local);
        if (new_local_range == NULL) {
            goto cleanup;
        }

        *new_local_range = *local_range;
        // NOTE: Not present in vinix, in case of weird VMM bugs keep this line in mind :^)
        new_local_range->pagemap = new_pagemap;

        if (global_range->res != NULL) {
            global_range->res->refcount++;
        }

        if ((local_range->flags & MAP_SHARED) != 0) {
            VECTOR_PUSH_BACK(&global_range->locals, new_local_range);
            for (uintptr_t i = local_range->base; i < local_range->base + local_range->length; i += PAGE_SIZE) {
                uint64_t *old_pte = vmm_virt2pte(pagemap, i, false);
                if (old_pte == NULL) {
                    continue;
                }

                uint64_t *new_pte = vmm_virt2pte(new_pagemap, i, true);
                if (new_pte == NULL) {
                    goto cleanup;
                }
                *new_pte = *old_pte;
            }
        } else {
            struct mmap_range_global *new_global_range = ALLOC(struct mmap_range_global);
            if (new_global_range == NULL) {
                goto cleanup;
            }

            new_global_range->shadow_pagemap = vmm_new_pagemap();
            if (new_global_range->shadow_pagemap == NULL) {
                goto cleanup;
            }

            new_global_range->base = global_range->base;
            new_global_range->length = global_range->length;
            new_global_range->res = global_range->res;
            new_global_range->offset = global_range->offset;

            VECTOR_PUSH_BACK(&new_global_range->locals, new_local_range);

            if ((local_range->flags & MAP_ANONYMOUS) != 0) {
                for (uintptr_t i = local_range->base; i < local_range->base + local_range->length; i += PAGE_SIZE) {
                    uint64_t *old_pte = vmm_virt2pte(pagemap, i, false);
                    if (old_pte == NULL || (PTE_GET_FLAGS(*old_pte) & PTE_PRESENT) == 0) {
                        continue;
                    }

                    uint64_t *new_pte = vmm_virt2pte(new_pagemap, i, true);
                    if (new_pte == NULL) {
                        goto cleanup;
                    }

                    uint64_t *new_spte = vmm_virt2pte(new_global_range->shadow_pagemap, i, true);
                    if (new_spte == NULL) {
                        goto cleanup;
                    }

                    void *old_page = (void *)PTE_GET_ADDR(*old_pte);
                    void *page = pmm_alloc_nozero(1);
                    if (page == NULL) {
                        goto cleanup;
                    }

                    memcpy(page + VMM_HIGHER_HALF, old_page + VMM_HIGHER_HALF, PAGE_SIZE);
                    *new_pte = PTE_GET_FLAGS(*old_pte) | (uint64_t)page;
                    *new_spte = *new_pte;
                }
            } else {
                panic(NULL, true, "Non anon fork");
            }
        }

        VECTOR_PUSH_BACK(&new_pagemap->mmap_ranges, new_local_range);
    );

    spinlock_release(&pagemap->lock);
    return new_pagemap;

cleanup:
    spinlock_release(&pagemap->lock);
    if (new_pagemap != NULL) {
        vmm_destroy_pagemap(new_pagemap);
    }
    return NULL;
}

static void destroy_level(uint64_t *pml, size_t start, size_t end, int level) {
    if (level == 0) {
        return;
    }

    for (size_t i = start; i < end; i++) {
        uint64_t *next_level = get_next_level(pml, i, false);
        if (next_level == NULL) {
            continue;
        }

        destroy_level(next_level, 0, 512, level - 1);
    }

    pmm_free((void *)pml - VMM_HIGHER_HALF, 1);
}

void vmm_destroy_pagemap(struct pagemap *pagemap) {
    //spinlock_acquire(&pagemap->lock);

    while (pagemap->mmap_ranges.length > 0) {
        struct mmap_range_local *local_range = pagemap->mmap_ranges.data[0];

        munmap(pagemap, local_range->base, local_range->length);
    }

    spinlock_acquire(&pagemap->lock);

    destroy_level(pagemap->top_level, 0, 256, 4);
    free(pagemap);
}

void vmm_switch_to(struct pagemap *pagemap) {
    asm volatile (
        "mov %0, %%cr3"
        :
        : "r" ((void *)pagemap->top_level - VMM_HIGHER_HALF)
        : "memory"
    );
}

bool vmm_map_page(struct pagemap *pagemap, uintptr_t virt, uintptr_t phys, uint64_t flags) {
    spinlock_acquire(&pagemap->lock);

    bool ok = false;
    size_t pml4_entry = (virt & (0x1ffull << 39)) >> 39;
    size_t pml3_entry = (virt & (0x1ffull << 30)) >> 30;
    size_t pml2_entry = (virt & (0x1ffull << 21)) >> 21;
    size_t pml1_entry = (virt & (0x1ffull << 12)) >> 12;

    uint64_t *pml4 = pagemap->top_level;
    uint64_t *pml3 = get_next_level(pml4, pml4_entry, true);
    if (pml3 == NULL) {
        goto cleanup;
    }
    uint64_t *pml2 = get_next_level(pml3, pml3_entry, true);
    if (pml2 == NULL) {
        goto cleanup;
    }
    uint64_t *pml1 = get_next_level(pml2, pml2_entry, true);
    if (pml1 == NULL) {
        goto cleanup;
    }

    if ((pml1[pml1_entry] & PTE_PRESENT) != 0) {
        errno = EINVAL;
        goto cleanup;
    }

    ok = true;
    pml1[pml1_entry] = phys | flags;

cleanup:
    spinlock_release(&pagemap->lock);
    return ok;
}

bool vmm_flag_page(struct pagemap *pagemap, bool lock, uintptr_t virt, uint64_t flags) {
    if (lock) {
        spinlock_acquire(&pagemap->lock);
    }

    bool ok = false;
    size_t pml4_entry = (virt & (0x1ffull << 39)) >> 39;
    size_t pml3_entry = (virt & (0x1ffull << 30)) >> 30;
    size_t pml2_entry = (virt & (0x1ffull << 21)) >> 21;
    size_t pml1_entry = (virt & (0x1ffull << 12)) >> 12;

    uint64_t *pml4 = pagemap->top_level;
    uint64_t *pml3 = get_next_level(pml4, pml4_entry, false);
    if (pml3 == NULL) {
        goto cleanup;
    }
    uint64_t *pml2 = get_next_level(pml3, pml3_entry, false);
    if (pml2 == NULL) {
        goto cleanup;
    }
    uint64_t *pml1 = get_next_level(pml2, pml2_entry, false);
    if (pml1 == NULL) {
        goto cleanup;
    }

    if ((pml1[pml1_entry] & PTE_PRESENT) == 0) {
        errno = EINVAL;
        goto cleanup;
    }

    ok = true;
    pml1[pml1_entry] = PTE_GET_ADDR(pml1[pml1_entry]) | flags;

    asm volatile (
        "invlpg (%0)"
        :
        : "r" (virt)
        : "memory"
    );

cleanup:
    if (lock) {
        spinlock_release(&pagemap->lock);
    }
    return ok;
}

bool vmm_unmap_page(struct pagemap *pagemap, uintptr_t virt, bool already_locked) {
    if (!already_locked) {
        spinlock_acquire(&pagemap->lock);
    }

    bool ok = false;
    size_t pml4_entry = (virt & (0x1ffull << 39)) >> 39;
    size_t pml3_entry = (virt & (0x1ffull << 30)) >> 30;
    size_t pml2_entry = (virt & (0x1ffull << 21)) >> 21;
    size_t pml1_entry = (virt & (0x1ffull << 12)) >> 12;

    uint64_t *pml4 = pagemap->top_level;
    uint64_t *pml3 = get_next_level(pml4, pml4_entry, false);
    if (pml3 == NULL) {
        goto cleanup;
    }
    uint64_t *pml2 = get_next_level(pml3, pml3_entry, false);
    if (pml2 == NULL) {
        goto cleanup;
    }
    uint64_t *pml1 = get_next_level(pml2, pml2_entry, false);
    if (pml1 == NULL) {
        goto cleanup;
    }

    if ((pml1[pml1_entry] & PTE_PRESENT) == 0) {
        errno = EINVAL;
        goto cleanup;
    }

    ok = true;
    pml1[pml1_entry] = 0;

    asm volatile (
        "invlpg (%0)"
        :
        : "r" (virt)
        : "memory"
    );

cleanup:
    if (!already_locked) {
        spinlock_release(&pagemap->lock);
    }
    return ok;
}

uint64_t *vmm_virt2pte(struct pagemap *pagemap, uintptr_t virt, bool allocate) {
    size_t pml4_entry = (virt & (0x1ffull << 39)) >> 39;
    size_t pml3_entry = (virt & (0x1ffull << 30)) >> 30;
    size_t pml2_entry = (virt & (0x1ffull << 21)) >> 21;
    size_t pml1_entry = (virt & (0x1ffull << 12)) >> 12;

    uint64_t *pml4 = pagemap->top_level;
    uint64_t *pml3 = get_next_level(pml4, pml4_entry, allocate);
    if (pml3 == NULL) {
        return NULL;
    }
    uint64_t *pml2 = get_next_level(pml3, pml3_entry, allocate);
    if (pml2 == NULL) {
        return NULL;
    }
    uint64_t *pml1 = get_next_level(pml2, pml2_entry, allocate);
    if (pml1 == NULL) {
        return NULL;
    }

    return &pml1[pml1_entry];
}

uintptr_t vmm_virt2phys(struct pagemap *pagemap, uintptr_t virt) {
    uint64_t *pte = vmm_virt2pte(pagemap, virt, false);
    if (pte == NULL || (PTE_GET_FLAGS(*pte) & PTE_PRESENT) == 0) {
        return INVALID_PHYS;
    }

    return PTE_GET_ADDR(*pte);
}
