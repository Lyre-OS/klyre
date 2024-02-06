#ifndef _MM__PMM_K_H
#define _MM__PMM_K_H

#include <stddef.h>
#include <limine.h>

extern volatile struct limine_memmap_request memmap_request;

void pmm_init(void);
void *pmm_alloc(size_t pages);
void *pmm_alloc_nozero(size_t pages);
void pmm_free(void *addr, size_t pages);

#endif
