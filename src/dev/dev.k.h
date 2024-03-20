#ifndef DEV__DEV_K_H_
#define DEV__DEV_K_H_

#include <stdint.h>
#include <dev/pci.k.h>
#include <lib/misc.k.h>

#define DRIVER_PCI 1

struct driver {
    int type;
    
    union {
        struct pci_driver *pci_dev;
    };
};

extern symbol drivers_start;
extern symbol drivers_end;

#define EXPORT_PCI_DRIVER(drv) \
    __attribute__((used, section(".drivers"))) \
    static struct driver __##drv = { .type = DRIVER_PCI, .pci_dev = &drv }

#define DRIVER_FOR_EACH(TYPE, BINDING, ...) do { \
    for (struct driver *BINDING = (struct driver *)drivers_start; (uintptr_t)BINDING < (uintptr_t)drivers_end; BINDING++) { \
        if (BINDING->type != TYPE) { \
            continue; \
        } \
        __VA_ARGS__ \
    } \
} while(0)

void dev_init(void);

#endif
