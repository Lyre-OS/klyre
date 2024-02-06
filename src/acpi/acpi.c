#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <lib/libc.k.h>
#include <lib/panic.k.h>
#include <lib/print.k.h>
#include <mm/vmm.k.h>
#include <acpi/acpi.k.h>
#include <acpi/madt.k.h>

static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

struct rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum;
    char reserved[3];
};

struct rsdt {
    struct sdt;
    char data[];
};

static struct rsdp *rsdp = NULL;
static struct rsdt *rsdt = NULL;

static inline bool use_xsdt(void) {
    return rsdp->revision >= 2 && rsdp->xsdt_addr != 0;
}

void acpi_init(void) {
    struct limine_rsdp_response *rsdp_resp = rsdp_request.response;
    if (rsdp_resp == NULL || rsdp_resp->address == NULL) {
        panic(NULL, true, "ACPI is not supported on this machine");
    }

    rsdp = rsdp_resp->address;

    if (use_xsdt()) {
        rsdt = (struct rsdt *)(rsdp->xsdt_addr + VMM_HIGHER_HALF);
    } else {
        rsdt = (struct rsdt *)((uint64_t)rsdp->rsdt_addr + VMM_HIGHER_HALF);
    }

    kernel_print("acpi: Revision: %lu\n", rsdp->revision);
    kernel_print("acpi: Uses XSDT? %s\n", use_xsdt() ? "true" : "false");
    kernel_print("acpi: RSDT at %lx\n", rsdt);

    struct sdt *fadt = acpi_find_sdt("FACP", 0);
    if (fadt != NULL && fadt->length >= 116) {
        uint32_t fadt_flags = *((uint32_t *)fadt + 28);

        if ((fadt_flags & (1 << 20)) != 0) {
            panic(NULL, true, "Lyre does not support HW reduced ACPI systems");
        }
    }

    madt_init();
}

void *acpi_find_sdt(const char signature[static 4], size_t index) {
    size_t entry_count = (rsdt->length - sizeof(struct sdt)) / (use_xsdt() ? 8 : 4);

    for (size_t i = 0; i < entry_count; i++) {
        struct sdt *sdt = NULL;
        if (use_xsdt()) {
            sdt = (struct sdt *)(*((uint64_t*)rsdt->data + i) + VMM_HIGHER_HALF);
        } else {
            sdt = (struct sdt *)(*((uint32_t*)rsdt->data + i) + VMM_HIGHER_HALF);
        }

        if (memcmp(sdt->signature, signature, 4) != 0) {
            continue;
        }

        if (index > 0) {
            index--;
            continue;
        }

        kernel_print("acpi: Found '%4s' at %lx, length=%lu\n", signature, sdt, sdt->length);
        return sdt;
    }

    kernel_print("acpi: Could not find '%4s'\n", signature);
    return NULL;
}
