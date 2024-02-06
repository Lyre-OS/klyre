#ifndef _MM__SLAB_K_H
#define _MM__SLAB_K_H

#include <stddef.h>

void slab_init(void);
void *slab_alloc(size_t size);
void *slab_realloc(void *addr, size_t size);
void slab_free(void *addr);

#endif
