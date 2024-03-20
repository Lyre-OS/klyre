#ifndef MM__SLAB_K_H_
#define MM__SLAB_K_H_

#include <stddef.h>

void slab_init(void);
void *slab_alloc(size_t size);
void *slab_realloc(void *addr, size_t size);
void slab_free(void *addr);

#endif
