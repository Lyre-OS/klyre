#ifndef LIB__ALLOC_K_H_
#define LIB__ALLOC_K_H_

#include <stddef.h>
#include <mm/slab.k.h>

#define ALLOC(TYPE) (alloc(sizeof(TYPE)))

static inline void *alloc(size_t size) {
    return slab_alloc(size);
}

static inline void *realloc(void *addr, size_t size) {
    return slab_realloc(addr, size);
}

static inline void free(void *addr) {
    slab_free(addr);
}

#endif
