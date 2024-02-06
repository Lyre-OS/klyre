#ifndef _DEV__PCI_K_H
#define _DEV__PCI_K_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct pci_device {
    uint8_t seg;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;

    uint8_t pci_class;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t rev_id;
    uint16_t device_id;
    uint16_t vendor_id;

    bool msi_supported, msix_supported;
    uint16_t msi_offset, msix_offset;
    bool pcie_supported;
    uint16_t pcie_offset; 
};

struct pci_bar {
    uintptr_t base;
    size_t len;
    bool is_mmio;
};

struct pci_driver {
    const char *name;
    int match;

    void (*init)(struct pci_device *);

    uint8_t pci_class, subclass, prog_if;
    uint16_t vendor;
    uint8_t devcount;
    uint16_t devices[];
};

#define PCI_MATCH_CLASS (1 << 0)
#define PCI_MATCH_SUBCLASS (1 << 1)
#define PCI_MATCH_PROG_IF (1 << 2)
#define PCI_MATCH_DEVICE (1 << 3)
#define PCI_MATCH_VENDOR (1 << 4)

#define PCI_PRIV_PIO 0x1
#define PCI_PRIV_MMIO 0x2
#define PCI_PRIV_BUSMASTER 0x4

#define PCI_READD(DEV, OFF) pci_read(DEV, OFF, 4)
#define PCI_READW(DEV, OFF) pci_read(DEV, OFF, 2)
#define PCI_READB(DEV, OFF) pci_read(DEV, OFF, 1)
#define PCI_WRITED(DEV, OFF, VAL) pci_write(DEV, OFF, VAL, 4)
#define PCI_WRITEW(DEV, OFF, VAL) pci_write(DEV, OFF, VAL, 2)
#define PCI_WRITEB(DEV, OFF, VAL) pci_write(DEV, OFF, VAL, 1)

extern uint32_t (*pci_read)(struct pci_device *, uint32_t, int);
extern void (*pci_write)(struct pci_device *, uint32_t, uint32_t, int);

bool pci_map_bar(struct pci_bar bar);
void pci_set_privl(struct pci_device *d, uint16_t privilege);
bool pci_setmask(struct pci_device *d, size_t index, bool masked);
bool pci_enable_irq(struct pci_device *d, size_t index, int vec);
struct pci_bar pci_get_bar(struct pci_device *d, uint8_t index);

struct pci_device *pci_get_device(uint8_t class, uint8_t subclass, uint8_t prog_if, size_t index);
struct pci_device *pci_get_device_by_vendor(uint16_t vendor, uint16_t id, size_t index);
void pci_init(void);

#endif
