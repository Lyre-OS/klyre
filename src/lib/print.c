#include <stdbool.h>
#include <stdint.h>
#include <dev/char/serial.k.h>
#include <dev/char/console.k.h>
#include <lib/libc.k.h>
#include <lib/lock.k.h>
#include <lib/print.k.h>
#include <lib/debug.k.h>
#include <sched/sched.k.h>
#include <sys/cpu.k.h>
#include <printf/printf.h>

// Needed for eyalroz printf
void putchar_(char _) {
    (void)_;
}

static spinlock_t print_lock = SPINLOCK_INIT;

void kernel_vprint(const char *fmt, va_list args) {
    spinlock_acquire(&print_lock);

    char buffer[1024];
    size_t length = vsnprintf(buffer, sizeof(buffer), fmt, args);

    serial_outstr(buffer);
    console_write(buffer, length);
    spinlock_release(&print_lock);
}

void kernel_print(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kernel_vprint(fmt, args);
    va_end(args);
}

spinlock_t debug_print_lock = SPINLOCK_INIT;
bool debug_on = debug;

void debug_vprint(size_t indent, const char *fmt, va_list args) {
    if (debug_on) {
        bool ints = interrupt_toggle(false);

        spinlock_acquire(&debug_print_lock);

        serial_out('\n');

        for (size_t i = 0; i < indent; i++) {
            serial_out('\t');
        }

        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        serial_outstr(buffer);
        spinlock_release(&debug_print_lock);

        interrupt_toggle(ints);
    }
}

void debug_print(size_t indent, const char *fmt, ...) {
    if (debug_on) {
        va_list args;
        va_start(args, fmt);
        debug_vprint(indent, fmt, args);
        va_end(args);
    }
}

int syscall_debug(void *_, const char *str) {
    (void)_;

    DEBUG_SYSCALL_ENTER("debug(\"%s\")", str);
    DEBUG_SYSCALL_LEAVE("%d", 0);

    return 0;
}
