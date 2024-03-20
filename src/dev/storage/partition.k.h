#ifndef DEV__STORAGE__PARTITION_K_H_
#define DEV__STORAGE__PARTITION_K_H_

#include <lib/resource.k.h>
#include <stdint.h>

void partition_enum(struct resource *root, const char *rootname, uint16_t blocksize, const char *convention);

#endif
