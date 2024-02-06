#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/idt.k.h>
#include <lib/lock.k.h>
#include <lib/panic.k.h>
#include <lib/print.k.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
};

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];

void *isr[256];
uint8_t idt_panic_ipi_vector;

void idt_register_handler(uint8_t vector, void *handler, uint8_t flags) {
    uint64_t handler_int = (uint64_t)handler;

    idt[vector] = (struct idt_entry){
        .offset_low = (uint16_t)handler_int,
        .selector = 0x28,
        .ist = 0,
        .flags = flags,
        .offset_mid = (uint16_t)(handler_int >> 16),
        .offset_hi = (uint32_t)(handler_int >> 32),
        .reserved = 0
    };
}

uint8_t idt_allocate_vector(void) {
    static spinlock_t lock = SPINLOCK_INIT;
    static uint8_t free_vector = 32;

    spinlock_acquire(&lock);

    if (free_vector == 0xf0) {
        panic(NULL, true, "IDT exhausted");
    }

    uint8_t ret = free_vector++;

    spinlock_release(&lock);

    return ret;
}

void idt_set_ist(uint8_t vector, uint8_t ist) {
    idt[vector].ist = ist;
}

uint8_t idt_get_ist(uint8_t vector) {
    return idt[vector].ist;
}

void idt_set_flags(uint8_t vector, uint8_t flags) {
    idt[vector].flags = flags;
}

void idt_reload(void) {
    struct idtr idtr = {
        .limit = sizeof(idt) - 1,
        .base = (uint64_t)idt
    };

    asm volatile ("lidt %0" :: "m"(idtr) : "memory");
}

extern void *isr_thunks[];

extern void panic_ipi_entry(void);

static void generic_isr(uint8_t vector, struct cpu_ctx *ctx) {
    panic(ctx, false, "Generic ISR triggered on vector %u", vector);
}

void idt_init(void) {
    idt_panic_ipi_vector = idt_allocate_vector();

    for (size_t i = 0; i < 256; i++) {
        if (i == idt_panic_ipi_vector) {
            idt_register_handler(i, panic_ipi_entry, 0x8e);
        } else {
            idt_register_handler(i, isr_thunks[i], 0x8e);
            isr[i] = generic_isr;
        }
    }

    idt_reload();
}
