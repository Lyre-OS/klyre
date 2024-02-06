#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <dev/lapic.k.h>
#include <lib/panic.k.h>
#include <lib/print.k.h>
#include <lib/trace.k.h>
#include <sys/cpu.k.h>
#include <sys/idt.k.h>

static spinlock_t panic_lock = SPINLOCK_INIT;

volatile size_t panic_cpu_counter = 0;

noreturn void panic(struct cpu_ctx *ctx, bool trace, const char *fmt, ...) {
    interrupt_toggle(false);

    if (smp_started) {
        asm volatile ("lock incq (%0)" :: "r"(&panic_cpu_counter) : "memory" );

        spinlock_acquire_no_dead_check(&panic_lock);

        lapic_send_ipi(0, idt_panic_ipi_vector | 0b10 << 18);

        while (panic_cpu_counter != cpu_count) {}
    }

    // Force unlock the print lock
    spinlock_release(&debug_print_lock);

    debug_on = true;

    debug_print(0, "\n\n*** LYRE PANIC ***\n");
    debug_print(0, "The Lyre kernel panicked with the following message:  ");

    va_list args;
    va_start(args, fmt);

    debug_vprint(0, fmt, args);
    debug_print(0, "\n");

    if (ctx == NULL) {
        goto halt;
    }

    uint64_t cr2 = read_cr2();
    uint64_t cr3 = read_cr3();
    debug_print(0, "CPU context at panic:");
    debug_print(0,  "  RAX=%016lx  RBX=%016lx\n"
                 "  RCX=%016lx  RDX=%016lx\n"
                 "  RSI=%016lx  RDI=%016lx\n"
                 "  RBP=%016lx  RSP=%016lx\n"
                 "  R08=%016lx  R09=%016lx\n"
                 "  R10=%016lx  R11=%016lx\n"
                 "  R12=%016lx  R13=%016lx\n"
                 "  R14=%016lx  R15=%016lx\n"
                 "  RIP=%016lx  RFLAGS=%08lx\n"
                 "  CS=%04lx DS=%04lx ES=%04lx SS=%04lx\n"
                 "  CR2=%016lx  CR3=%016lx\n"
                 "  ERR=%016lx",
                 ctx->rax, ctx->rbx, ctx->rcx, ctx->rdx,
                 ctx->rsi, ctx->rdi, ctx->rbp, ctx->rsp,
                 ctx->r8, ctx->r9, ctx->r10, ctx->r11,
                 ctx->r12, ctx->r13, ctx->r14, ctx->r15,
                 ctx->rip, ctx->rflags,
                 ctx->cs, ctx->ds, ctx->es, ctx->ss,
                 cr2, cr3, ctx->err);

    debug_print(0, "\n");

halt:
    if (trace && (ctx == NULL || ctx->cs == 0x28)) {
        debug_print(0, "Stacktrace follows:");
        trace_printstack(ctx == NULL ? 0 : ctx->rbp);
        debug_print(0, "\n");
    }

    debug_print(0, "System halted.");

    for (;;) {
        halt();
    }
}
