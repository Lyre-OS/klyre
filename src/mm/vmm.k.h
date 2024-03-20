#ifndef MM__VMM_K_H_
#define MM__VMM_K_H_

#include <stdbool.h>
#include <stdint.h>
#include <limine.h>
#include <lib/lock.k.h>
#include <lib/vector.k.h>

#define PAGE_SIZE 4096

#define PTE_PRESENT (1ull << 0ull)
#define PTE_WRITABLE (1ull << 1ull)
#define PTE_USER (1ull << 2ull)
#define PTE_NX (1ull << 63ull)

#define PTE_ADDR_MASK 0x000ffffffffff000
#define PTE_GET_ADDR(VALUE) ((VALUE) & PTE_ADDR_MASK)
#define PTE_GET_FLAGS(VALUE) ((VALUE) & ~PTE_ADDR_MASK)

struct pagemap {
    spinlock_t lock;
    uint64_t *top_level;
    VECTOR_TYPE(struct mmap_range_local *) mmap_ranges;
};

extern volatile struct limine_hhdm_request hhdm_request;

extern struct pagemap *vmm_kernel_pagemap;
extern bool vmm_initialised;

#define VMM_HIGHER_HALF (hhdm_request.response->offset)
#define INVALID_PHYS ((uint64_t)0xffffffffffffffff)

void vmm_init(void);

struct pagemap *vmm_new_pagemap(void);
struct pagemap *vmm_fork_pagemap(struct pagemap *pagemap);
void vmm_destroy_pagemap(struct pagemap *pagemap);
void vmm_switch_to(struct pagemap *pagemap);
bool vmm_map_page(struct pagemap *pagemap, uintptr_t virt, uintptr_t phys, uint64_t flags);
bool vmm_flag_page(struct pagemap *pagemap, bool lock, uintptr_t virt, uint64_t flags);
bool vmm_unmap_page(struct pagemap *pagemap, uintptr_t virt, bool already_locked);
uint64_t *vmm_virt2pte(struct pagemap *pagemap, uintptr_t virt, bool allocate);
uintptr_t vmm_virt2phys(struct pagemap *pagemap, uintptr_t virt);

#endif
