#include <stdbool.h>
#include <stdint.h>
#include <dev/char/serial.k.h>
#include <lib/lock.k.h>
#include <sys/port.k.h>

static const uint16_t com1_port = 0x3f8;
static const uint16_t com_ports[4] = {com1_port, 0x2f8, 0x3e8, 0x2e8};

static bool initialize_port(uint16_t port) {
    // Check if the port exists by writing and read from the scratch register.
    outb(port + 7, 0x69);
    if (inb(port + 7) != 0x69) {
        return false;
    }

    // Enable data available interrupts and enable DLAB.
    outb(port + 1, 0x01);
    outb(port + 3, 0x80);

    // Set divisor to low 1 high 0 (115200 baud)
    outb(port + 0, 0x01);
    outb(port + 1, 0x00);

    // Enable FIFO and interrupts
    outb(port + 3, 0x03);
    outb(port + 2, 0xC7);
    outb(port + 4, 0x0b);

    return true;
}

static inline bool is_transmitter_empty(uint16_t port) {
    return (inb(port + 5) & 0b01000000) != 0;
}

static inline void transmit_data(uint16_t port, uint8_t value) {
    while (!is_transmitter_empty(port)) {
        asm volatile ("pause");
    }
    outb(port, value);
}

void serial_init(void) {
    for (int i = 0; i < (int)(sizeof(com_ports) / sizeof(com_ports[0])); i++) {
        initialize_port(com_ports[i]);
    }
}

void serial_out(char ch) {
    static spinlock_t lock = SPINLOCK_INIT;

    spinlock_acquire(&lock);

    if (ch == '\n') {
        transmit_data(com1_port, '\r');
    }
    transmit_data(com1_port, ch);

    spinlock_release(&lock);
}

void serial_outstr(const char *str) {
    static spinlock_t lock = SPINLOCK_INIT;

    spinlock_acquire(&lock);

    while (*str) {
        if (*str == '\n') {
            transmit_data(com1_port, '\r');
        }
        transmit_data(com1_port, *str++);
    }

    spinlock_release(&lock);
}
