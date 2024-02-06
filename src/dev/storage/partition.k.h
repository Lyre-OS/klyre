#ifndef _DEV__STORAGE__PARTITION_K_H
#define _DEV__STORAGE__PARTITION_K_H

#include <lib/resource.k.h>
#include <stdint.h>

void partition_enum(struct resource *root, const char *rootname, uint16_t blocksize, const char *convention);

#endif
