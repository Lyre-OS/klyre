#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <lib/bitmap.k.h>
#include <lib/libc.k.h>
#include <lib/lock.k.h>
#include <lib/misc.k.h>
#include <lib/print.k.h>
#include <mm/pmm.k.h>
#include <mm/vmm.k.h>

volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

static spinlock_t lock = SPINLOCK_INIT;
static uint8_t *bitmap = NULL;
static uint64_t highest_page_index = 0;
static uint64_t last_used_index = 0;
static uint64_t usable_pages = 0;
static uint64_t used_pages = 0;
static uint64_t reserved_pages = 0;

void pmm_init(void) {
    // TODO: Check if memmap and hhdm responses are null and panic
    struct limine_memmap_response *memmap = memmap_request.response;
    struct limine_hhdm_response *hhdm = hhdm_request.response;
    struct limine_memmap_entry **entries = memmap->entries;

    uint64_t highest_addr = 0;

    // Calculate how big the memory map needs to be.
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = entries[i];

        kernel_print("pmm: Memory map entry: base=%lx, length=%lx, type=%lx\n",
            entry->base, entry->length, entry->type);

        switch (entry->type) {
            case LIMINE_MEMMAP_USABLE:
                usable_pages += DIV_ROUNDUP(entry->length, PAGE_SIZE);
                highest_addr = MAX(highest_addr, entry->base + entry->length);
                break;
            case LIMINE_MEMMAP_RESERVED:
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            case LIMINE_MEMMAP_ACPI_NVS:
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            case LIMINE_MEMMAP_KERNEL_AND_MODULES:
                reserved_pages += DIV_ROUNDUP(entry->length, PAGE_SIZE);
                break;
        }
    }

    // Calculate the needed size for the bitmap in bytes and align it to page size.
    highest_page_index = highest_addr / PAGE_SIZE;
    uint64_t bitmap_size = ALIGN_UP(highest_page_index / 8, PAGE_SIZE);

    kernel_print("pmm: Highest address: %lx\n", highest_addr);
    kernel_print("pmm: Bitmap size: %lu bytes\n", bitmap_size);

    // Find a hole for the bitmap in the memory map.
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        if (entry->length >= bitmap_size) {
            bitmap = (uint8_t *)(entry->base + hhdm->offset);

            // Initialise entire bitmap to 1 (non-free)
            memset(bitmap, 0xff, bitmap_size);

            entry->length -= bitmap_size;
            entry->base += bitmap_size;

            break;
        }
    }

    // Populate free bitmap entries according to the memory map.
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        for (uint64_t j = 0; j < entry->length; j += PAGE_SIZE) {
            bitmap_reset(bitmap, (entry->base + j) / PAGE_SIZE);
        }
    }

    kernel_print("pmm: Usable memory: %luMiB\n", (usable_pages * 4096) / 1024 / 1024);
    kernel_print("pmm: Reserved memory: %luMiB\n", (reserved_pages * 4096) / 1024 / 1024);
}

static void *inner_alloc(size_t pages, uint64_t limit) {
    size_t p = 0;

    while (last_used_index < limit) {
        if (!bitmap_test(bitmap, last_used_index++)) {
            if (++p == pages) {
                size_t page = last_used_index - pages;
                for (size_t i = page; i < last_used_index; i++) {
                    bitmap_set(bitmap, i);
                }
                return (void *)(page * PAGE_SIZE);
            }
        } else {
            p = 0;
        }
    }

    return NULL;
}

void *pmm_alloc(size_t pages) {
    void *ret = pmm_alloc_nozero(pages);
    if (ret != NULL) {
        memset(ret + VMM_HIGHER_HALF, 0, pages * PAGE_SIZE);
    }

    return ret;
}

void *pmm_alloc_nozero(size_t pages) {
    spinlock_acquire(&lock);

    size_t last = last_used_index;
    void *ret = inner_alloc(pages, highest_page_index);

    if (ret == NULL) {
        last_used_index = 0;
        ret = inner_alloc(pages, last);
    }

    // TODO: Check if ret is null and panic
    used_pages += pages;

    spinlock_release(&lock);
    return ret;
}

void pmm_free(void *addr, size_t pages) {
    spinlock_acquire(&lock);

    size_t page = (uint64_t)addr / PAGE_SIZE;
    for (size_t i = page; i < page + pages; i++) {
        bitmap_reset(bitmap, i);
    }
    used_pages -= pages;

    spinlock_release(&lock);
}
