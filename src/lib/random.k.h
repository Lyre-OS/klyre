#ifndef _LIB__RANDOM_K_H
#define _LIB__RANDOM_K_H

#include <stddef.h>
#include <stdint.h>

void random_init(void);
void random_seed(uint64_t seed);
void random_fill(void *buf, size_t length);
uint64_t random_generate();

#endif
