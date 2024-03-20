#ifndef FS__DEVTMPFS_K_H_
#define FS__DEVTMPFS_K_H_

#include <stdbool.h>
#include <lib/resource.k.h>

void devtmpfs_init(void);
bool devtmpfs_add_device(struct resource *device, const char *name);

#endif
