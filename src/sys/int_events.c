#include <stdint.h>
#include <sys/int_events.k.h>
#include <sys/idt.k.h>
#include <sys/cpu.k.h>
#include <lib/event.k.h>
#include <dev/lapic.k.h>

struct event int_events[256];

static void int_events_handler(uint8_t vector, struct cpu_ctx *ctx) {
    (void)ctx;
    event_trigger(&int_events[vector], false);
    lapic_eoi();
}

void int_events_init(void) {
    for (size_t i = 32; i < 0xef; i++) {
        isr[i] = int_events_handler;
    }
}
