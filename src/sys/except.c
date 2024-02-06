#include <stdint.h>
#include <stddef.h>
#include <mm/mmap.k.h>
#include <sys/except.k.h>
#include <sys/idt.k.h>
#include <lib/misc.k.h>
#include <lib/print.k.h>
#include <lib/panic.k.h>

static const char *exceptions[] = {
    "Division exception",
    "Debug",
    "NMI",
    "Breakpoint",
    "Overflow",
    "Bound range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "???",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "???",
    "x87 exception",
    "Alignment check",
    "Machine check",
    "SIMD exception",
    "Virtualisation"
};

static void exception_handler(uint8_t vector, struct cpu_ctx *ctx) {
    if (vector == 0xe && mmap_handle_pf(ctx)) {
        return;
    }

    panic(ctx, true, "Exception %s triggered (vector %d)", exceptions[vector], vector);

    __builtin_unreachable();
}

void except_init(void) {
    for (size_t i = 0; i < SIZEOF_ARRAY(exceptions); i++) {
        isr[i] = exception_handler;
    }

    idt_set_ist(0xe, 2); // #PF uses IST 2
}
