#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <lib/alloc.k.h>
#include <lib/errno.k.h>
#include <lib/lock.k.h>
#include <lib/misc.k.h>
#include <lib/print.k.h>
#include <lib/resource.k.h>
#include <lib/vector.k.h>
#include <lib/debug.k.h>
#include <mm/mmap.k.h>
#include <mm/pmm.k.h>
#include <mm/vmm.k.h>
#include <sched/proc.k.h>
#include <sys/cpu.k.h>

struct addr2range {
    struct mmap_range_local *range;
    size_t memory_page;
    size_t file_page;
};

struct addr2range addr2range(struct pagemap *pagemap, uintptr_t virt) {
    VECTOR_FOR_EACH(&pagemap->mmap_ranges, it,
        struct mmap_range_local *local_range = *it;
        if (virt < local_range->base || virt >= local_range->base + local_range->length) {
            continue;
        }

        size_t memory_page = virt / PAGE_SIZE;
        size_t file_page = local_range->offset / PAGE_SIZE + (memory_page - local_range->base / PAGE_SIZE);
        return (struct addr2range){.range = local_range, .memory_page = memory_page, .file_page = file_page};
    );

    return (struct addr2range){.range = NULL, .memory_page = 0, .file_page = 0};
}

void mmap_list_ranges(struct pagemap *pagemap) {
    kernel_print("Ranges for %lx:\n", pagemap);

    VECTOR_FOR_EACH(&pagemap->mmap_ranges, it,
        struct mmap_range_local *local_range = *it;
        kernel_print("\tbase=%lx, length=%lx, offset=%lx\n", local_range->base, local_range->length, local_range->offset);
    );
}

bool mmap_handle_pf(struct cpu_ctx *ctx) {
    if ((ctx->err & 0x1) != 0) {
        return false;
    }

    // TODO: mmap can be expensive, consider enabling interrupts
    // temporarily
    uint64_t cr2 = read_cr2();

    struct thread *thread = sched_current_thread();
    struct process *process = thread->process;
    struct pagemap *pagemap = process->pagemap;

    spinlock_acquire(&pagemap->lock);

    struct addr2range range = addr2range(pagemap, cr2);
    struct mmap_range_local *local_range = range.range;

    spinlock_release(&pagemap->lock);

    if (local_range == NULL) {
        return false;
    }

    void *page = NULL;
    if ((local_range->flags & MAP_ANONYMOUS) != 0) {
        page = pmm_alloc(1);
    } else {
        struct resource *res = page = local_range->global->res;
        page = res->mmap(res, range.file_page, local_range->flags);
    }

    if (page == NULL) {
        return false;
    }

    return mmap_page_in_range(local_range->global, range.memory_page * PAGE_SIZE, (uintptr_t)page, local_range->prot);
}

bool mmap_page_in_range(struct mmap_range_global *global, uintptr_t virt,
                            uintptr_t phys, int prot) {
    uint64_t pt_flags = PTE_PRESENT | PTE_USER;

    if ((prot & PROT_WRITE) != 0) {
        pt_flags |= PTE_WRITABLE;
    }
    if ((prot & PROT_EXEC) == 0) {
        pt_flags |= PTE_NX;
    }

    if (!vmm_map_page(global->shadow_pagemap, virt, phys, pt_flags)) {
        return false;
    }

    VECTOR_FOR_EACH(&global->locals, it,
        struct mmap_range_local *local_range = *it;
        if (virt < local_range->base || virt >= local_range->base + local_range->length) {
            continue;
        }

        if (!vmm_map_page(local_range->pagemap, virt, phys, pt_flags)) {
            return false;
        }
    );

    return true;
}

bool mmap_range(struct pagemap *pagemap, uintptr_t virt, uintptr_t phys,
                size_t length, int prot, int flags) {
    flags |= MAP_ANONYMOUS;

    uintptr_t aligned_virt = ALIGN_DOWN(virt, PAGE_SIZE);
    size_t aligned_length = ALIGN_UP(length + (virt - aligned_virt), PAGE_SIZE);

    struct mmap_range_global *global_range = NULL;
    struct mmap_range_local *local_range = NULL;

    global_range = ALLOC(struct mmap_range_global);
    if (global_range == NULL) {
        errno = ENOMEM;
        goto cleanup;
    }

    global_range->shadow_pagemap = vmm_new_pagemap();
    if (global_range->shadow_pagemap == NULL) {
        goto cleanup;
    }

    global_range->base = aligned_virt;
    global_range->length = aligned_length;

    local_range = ALLOC(struct mmap_range_local);
    if (local_range == NULL) {
        errno = ENOMEM;
        goto cleanup;
    }

    local_range->pagemap = pagemap;
    local_range->global = global_range;
    local_range->base = aligned_virt;
    local_range->length = aligned_length;
    local_range->prot = prot;
    local_range->flags = flags;

    VECTOR_PUSH_BACK(&global_range->locals, local_range);

    spinlock_acquire(&pagemap->lock);

    VECTOR_PUSH_BACK(&pagemap->mmap_ranges, local_range);

    spinlock_release(&pagemap->lock);

    for (size_t i = 0; i < aligned_length; i += PAGE_SIZE) {
        if (!mmap_page_in_range(global_range, aligned_virt + i, phys + i, prot)) {
            // FIXME: Page map is in inconsistent state at this point!
            goto cleanup;
        }
    }

    return true;

cleanup:
    if (local_range != NULL) {
        free(local_range);
    }
    if (global_range != NULL) {
        if (global_range->shadow_pagemap != NULL) {
            vmm_destroy_pagemap(global_range->shadow_pagemap);
        }

        free(global_range);
    }
    return false;
}

int mprotect(struct pagemap *pagemap, uintptr_t addr, size_t length, int prot) {
    int ret = -1;

    if (length == 0) {
        errno = EINVAL;
        goto cleanup;
    }
    length = ALIGN_UP(length, PAGE_SIZE);

    for (uintptr_t i = addr; i < addr + length; i += PAGE_SIZE) {
        struct mmap_range_local *local_range = addr2range(pagemap, i).range;

        if (local_range->prot == prot) {
            continue;
        }

        uintptr_t snip_begin = i;
        for (;;) {
            i += PAGE_SIZE;
            if (i >= local_range->base + local_range->length || i >= addr + length) {
                break;
            }
        }
        uintptr_t snip_end = i;
        uintptr_t snip_size = snip_end - snip_begin;

        spinlock_acquire(&pagemap->lock);

        if (snip_begin > local_range->base && snip_end < local_range->base + local_range->length) {
            struct mmap_range_local *postsplit_range = ALLOC(struct mmap_range_local);

            postsplit_range->pagemap = local_range->pagemap;
            postsplit_range->global = local_range->global;
            postsplit_range->base = snip_end;
            postsplit_range->length = (local_range->base + local_range->length) - snip_end;
            postsplit_range->offset = local_range->offset + (off_t)(snip_end - local_range->base);
            postsplit_range->prot = local_range->prot;
            postsplit_range->flags = local_range->flags;

            VECTOR_PUSH_BACK(&pagemap->mmap_ranges, postsplit_range);

            local_range->length -= postsplit_range->length;
        }

        for (uintptr_t j = snip_begin; j < snip_end; j += PAGE_SIZE) {
            uint64_t pt_flags = PTE_PRESENT | PTE_USER;

            if ((prot & PROT_WRITE) != 0) {
                pt_flags |= PTE_WRITABLE;
            }
            if ((prot & PROT_EXEC) == 0) {
                pt_flags |= PTE_NX;
            }

            vmm_flag_page(pagemap, false, j, pt_flags);
        }

        uintptr_t new_offset = local_range->offset + (snip_begin - local_range->base);

        if (snip_begin == local_range->base) {
            local_range->offset += snip_size;
            local_range->base = snip_end;
        }
        local_range->length -= snip_size;

        struct mmap_range_local *new_range = ALLOC(struct mmap_range_local);

        new_range->pagemap = local_range->pagemap;
        new_range->global = local_range->global;
        new_range->base = snip_begin;
        new_range->length = snip_size;
        new_range->offset = new_offset;
        new_range->prot = prot;
        new_range->flags = local_range->flags;

        VECTOR_PUSH_BACK(&pagemap->mmap_ranges, new_range);

        spinlock_release(&pagemap->lock);
    }

    ret = 0;

cleanup:
    return ret;
}

void *mmap(struct pagemap *pagemap, uintptr_t addr, size_t length, int prot,
           int flags, struct resource *res, off_t offset) {
    struct mmap_range_global *global_range = NULL;
    struct mmap_range_local *local_range = NULL;

    if (length == 0) {
        errno = EINVAL;
        goto cleanup;
    }
    length = ALIGN_UP(length, PAGE_SIZE);

    if ((flags & MAP_ANONYMOUS) == 0 && res != NULL && !res->can_mmap) {
        errno = ENODEV;
        return MAP_FAILED;
    }

    struct process *process = sched_current_thread()->process;

    uint64_t base = 0;
    if ((flags & MAP_FIXED) != 0) {
        if (!munmap(pagemap, addr, length)) {
            goto cleanup;
        }
        base = addr;
    } else {
        base = process->mmap_anon_base;
        process->mmap_anon_base += length + PAGE_SIZE;
    }

    global_range = ALLOC(struct mmap_range_global);
    if (global_range == NULL) {
        errno = ENOMEM;
        goto cleanup;
    }

    global_range->shadow_pagemap = vmm_new_pagemap();
    if (global_range->shadow_pagemap == NULL) {
        goto cleanup;
    }

    global_range->base = base;
    global_range->length = length;
    global_range->res = res;
    global_range->offset = offset;

    local_range = ALLOC(struct mmap_range_local);
    if (local_range == NULL) {
        goto cleanup;
    }

    local_range->pagemap = pagemap;
    local_range->global = global_range;
    local_range->base = base;
    local_range->length = length;
    local_range->prot = prot;
    local_range->flags = flags;
    local_range->offset = offset;

    VECTOR_PUSH_BACK(&global_range->locals, local_range);

    spinlock_acquire(&pagemap->lock);

    VECTOR_PUSH_BACK(&pagemap->mmap_ranges, local_range);

    spinlock_release(&pagemap->lock);

    if (res != NULL) {
        res->refcount++;
    }

    return (void *)base;

cleanup:
    if (local_range != NULL) {
        free(local_range);
    }
    if (global_range != NULL) {
        if (global_range->shadow_pagemap != NULL) {
            vmm_destroy_pagemap(global_range->shadow_pagemap);
        }

        free(global_range);
    }
    return MAP_FAILED;
}

bool munmap(struct pagemap *pagemap, uintptr_t addr, size_t length) {
    if (length == 0) {
        errno = EINVAL;
        return false;
    }
    length = ALIGN_UP(length, PAGE_SIZE);

    for (uintptr_t i = addr; i < addr + length; i += PAGE_SIZE) {
        struct addr2range range = addr2range(pagemap, i);
        if (range.range == NULL) {
            continue;
        }

        struct mmap_range_local *local_range = range.range;
        struct mmap_range_global *global_range = local_range->global;

        uintptr_t snip_begin = i;
        for (;;) {
            i += PAGE_SIZE;
            if (i >= local_range->base + local_range->length || i >= addr + length) {
                break;
            }
        }

        uintptr_t snip_end = i;
        size_t snip_length = snip_end - snip_begin;

        spinlock_acquire(&pagemap->lock);

        if (snip_begin > local_range->base && snip_end < local_range->base + local_range->length) {
            struct mmap_range_local *postsplit_range = ALLOC(struct mmap_range_local);
            if (postsplit_range == NULL) {
                // FIXME: Page map is in inconsistent state at this point!
                errno = ENOMEM;
                spinlock_release(&pagemap->lock);
                return false;
            }

            postsplit_range->pagemap = local_range->pagemap;
            postsplit_range->global = global_range;
            postsplit_range->base = snip_end;
            postsplit_range->length = (local_range->base + local_range->length) - snip_end;
            postsplit_range->offset = local_range->offset + (off_t)(snip_end - local_range->base);
            postsplit_range->prot = local_range->prot;
            postsplit_range->flags = local_range->flags;

            VECTOR_PUSH_BACK(&pagemap->mmap_ranges, postsplit_range);

            local_range->length -= postsplit_range->length;
        }

        for (uintptr_t j = snip_begin; j < snip_end; j += PAGE_SIZE) {
            vmm_unmap_page(pagemap, j, true);
        }

        if (snip_length == local_range->length) {
            VECTOR_REMOVE_BY_VALUE(&pagemap->mmap_ranges, local_range);
        }

        spinlock_release(&pagemap->lock);

        if (snip_length == local_range->length && global_range->locals.length == 1) {
            if ((local_range->flags & MAP_ANONYMOUS) != 0) {
                for (uintptr_t j = global_range->base; j < global_range->base + global_range->length; j += PAGE_SIZE) {
                    uintptr_t phys = vmm_virt2phys(global_range->shadow_pagemap, j);
                    if (phys == INVALID_PHYS) {
                        continue;
                    }

                    if (!vmm_unmap_page(global_range->shadow_pagemap, j, true)) {
                        // FIXME: Page map is in inconsistent state at this point!
                        errno = EINVAL;
                        return false;
                    }
                    pmm_free((void *)phys, 1);
                }
            } else {
                // TODO: res->unmap();
            }

            free(local_range);
        } else {
            if (snip_begin == local_range->base) {
                local_range->offset += snip_length;
                local_range->base = snip_end;
            }
            local_range->length -= snip_length;
        }
    }
    return true;
}

void *syscall_mmap(void *_, uintptr_t hint, size_t length, uint64_t flags, int fdnum, off_t offset) {
    (void)_;

    DEBUG_SYSCALL_ENTER("mmap(%lx, %lx, %lx, %d, %ld)", hint, length, flags, fdnum, offset);

    void *ret = MAP_FAILED;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct resource *res = NULL;
    if (fdnum != -1) {
        struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
        if (fd == NULL) {
            goto cleanup;
        }

        res = fd->description->res;
    } else if (offset != 0) {
        errno = EINVAL;
        goto cleanup;
    }
    ret = mmap(proc->pagemap, hint, length, (int)(flags >> 32), (int)flags, res, offset);

cleanup:
    DEBUG_SYSCALL_LEAVE("%llx", ret);
    return ret;
}

int syscall_munmap(void *_, uintptr_t addr, size_t length) {
    (void)_;

    DEBUG_SYSCALL_ENTER("munmap(%lx, %lx)", addr, length);

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    int ret = munmap(proc->pagemap, addr, length) ? 0 : -1;

    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_mprotect(void *_, uintptr_t addr, size_t length, int prot) {
    (void)_;

    DEBUG_SYSCALL_ENTER("mprotect(%lx, %lx, %x)", addr, length, prot);

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    int ret = mprotect(proc->pagemap, addr, length, prot);

    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}
