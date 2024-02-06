#include <stdint.h>
#include <dev/ps2.k.h>
#include <dev/lapic.k.h>
#include <dev/ioapic.k.h>
#include <sys/idt.k.h>
#include <sys/port.k.h>
#include <sys/cpu.k.h>

uint8_t ps2_keyboard_vector = 0;

uint8_t ps2_read(void) {
    while ((inb(0x64) & 1) == 0);
    return inb(0x60);
}

void ps2_write(uint16_t port, uint8_t value) {
    while ((inb(0x64) & 2) != 0);
    outb(port, value);
}

uint8_t ps2_read_config(void) {
    ps2_write(0x64, 0x20);
    return ps2_read();
}

void ps2_write_config(uint8_t value) {
    ps2_write(0x64, 0x60);
    ps2_write(0x60, value);
}

void ps2_init(void) {
    // Disable primary and secondary PS/2 ports
    ps2_write(0x64, 0xad);
    ps2_write(0x64, 0xa7);

    uint8_t ps2_config = ps2_read_config();
    // Enable keyboard interrupt and keyboard scancode translation
    ps2_config |= (1 << 0) | (1 << 6);
    // Enable mouse interrupt if any
    if ((ps2_config & (1 << 5)) != 0) {
        ps2_config |= (1 << 1);
    }
    ps2_write_config(ps2_config);

    // Enable keyboard port
    ps2_write(0x64, 0xae);
    // Enable mouse port if any
    if ((ps2_config & (1 << 5)) != 0) {
        ps2_write(0x64, 0xa8);
    }

    ps2_keyboard_vector = idt_allocate_vector();
    io_apic_set_irq_redirect(bsp_lapic_id, ps2_keyboard_vector, 1, true);
    inb(0x60);
}
