#ifndef _ACPI__ACPI_K_H
#define _ACPI__ACPI_K_H

#include <stddef.h>
#include <stdint.h>

struct sdt {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[6];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

void acpi_init(void);
void *acpi_find_sdt(const char signature[static 4], size_t index);

#endif
