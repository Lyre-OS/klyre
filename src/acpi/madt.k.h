#ifndef ACPI__MADT_K_H_
#define ACPI__MADT_K_H_

#include <stddef.h>
#include <stdint.h>
#include <lib/vector.k.h>

struct madt_header {
    uint8_t id;
    uint8_t length;
} __attribute__((packed));

struct madt_lapic {
    struct madt_header;
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed));

struct madt_io_apic {
    struct madt_header;
    uint8_t apic_id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsib;
} __attribute__((packed));

struct madt_iso {
    struct madt_header;
    uint8_t bus_source;
    uint8_t irq_source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

struct madt_nmi {
    struct madt_header;
    uint8_t processor;
    uint16_t flags;
    uint8_t lint;
} __attribute__((packed));

extern VECTOR_TYPE(struct madt_lapic *) madt_lapics;
extern VECTOR_TYPE(struct madt_io_apic *) madt_io_apics;
extern VECTOR_TYPE(struct madt_iso *) madt_isos;
extern VECTOR_TYPE(struct madt_nmi *) madt_nmis;

void madt_init(void);

#endif
