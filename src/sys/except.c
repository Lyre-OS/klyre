#include <stdint.h>
#include <stddef.h>
#include <stdnoreturn.h>
#include <mm/mmap.k.h>
#include <mm/vmm.k.h>
#include <sys/except.k.h>
#include <sys/idt.k.h>
#include <sys/cpu.k.h>
#include <sys/wait.h>
#include <lib/misc.k.h>
#include <lib/print.k.h>
#include <lib/panic.k.h>
#include <lib/resource.k.h>
#include <lib/event.k.h>
#include <sched/sched.k.h>

#define SIGILL  4
#define SIGBUS  7
#define SIGFPE  8
#define SIGKILL 9
#define SIGSEGV 11

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

static int exception_to_signal(uint8_t vector) {
    switch (vector) {
        case 0x00: return SIGFPE;   // #DE Division
        case 0x06: return SIGILL;   // #UD Invalid opcode
        case 0x0d: return SIGSEGV;  // #GP General protection
        case 0x0e: return SIGSEGV;  // #PF Page fault
        case 0x11: return SIGBUS;   // #AC Alignment check
        case 0x10: return SIGFPE;   // #MF x87 exception
        case 0x13: return SIGFPE;   // #XM SIMD exception
        default:   return SIGKILL;
    }
}

static noreturn void userspace_crash(uint8_t vector, struct cpu_ctx *ctx) {
    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;
    int signal = exception_to_signal(vector);

    uint64_t cr2 = read_cr2();

    kernel_print("\n*** USERSPACE CRASH ***\n");
    kernel_print("Process \"%s\" (PID %d) killed by %s (signal %d)\n",
        proc->name, proc->pid, exceptions[vector], signal);

    kernel_print("Registers:\n"
                 "  RAX=%016lx  RBX=%016lx\n"
                 "  RCX=%016lx  RDX=%016lx\n"
                 "  RSI=%016lx  RDI=%016lx\n"
                 "  RBP=%016lx  RSP=%016lx\n"
                 "  R08=%016lx  R09=%016lx\n"
                 "  R10=%016lx  R11=%016lx\n"
                 "  R12=%016lx  R13=%016lx\n"
                 "  R14=%016lx  R15=%016lx\n"
                 "  RIP=%016lx  RFLAGS=%08lx\n"
                 "  CS=%04lx SS=%04lx\n"
                 "  CR2=%016lx  ERR=%016lx\n",
                 ctx->rax, ctx->rbx, ctx->rcx, ctx->rdx,
                 ctx->rsi, ctx->rdi, ctx->rbp, ctx->rsp,
                 ctx->r8, ctx->r9, ctx->r10, ctx->r11,
                 ctx->r12, ctx->r13, ctx->r14, ctx->r15,
                 ctx->rip, ctx->rflags,
                 ctx->cs, ctx->ss,
                 cr2, ctx->err);

    // Print memory map with key address annotations
    kernel_print("Memory map:\n");

    struct pagemap *pagemap = proc->pagemap;
    VECTOR_FOR_EACH(&pagemap->mmap_ranges, it,
        struct mmap_range_local *local_range = *it;
        struct mmap_range_global *global = local_range->global;

        uintptr_t base = local_range->base;
        uintptr_t end = base + local_range->length;

        char prot_str[4] = "---";
        if (local_range->prot & PROT_READ)  prot_str[0] = 'r';
        if (local_range->prot & PROT_WRITE) prot_str[1] = 'w';
        if (local_range->prot & PROT_EXEC)  prot_str[2] = 'x';

        const char *type = (local_range->flags & MAP_ANONYMOUS) ? "anon" : "file";
        const char *name = global->name != NULL ? global->name : "";

        kernel_print("  %016lx-%016lx %s %s off=%lx %s\n",
            base, end, prot_str, type, local_range->offset, name);

        if (ctx->rip >= base && ctx->rip < end)
            kernel_print("    `-- \033[38;5;117mRIP (offset 0x%lx)\033[0m\n", ctx->rip - base + local_range->offset);
        if (ctx->rsp >= base && ctx->rsp < end)
            kernel_print("    `-- \033[38;5;150mRSP (offset 0x%lx)\033[0m\n", ctx->rsp - base + local_range->offset);
        if (ctx->rbp >= base && ctx->rbp < end)
            kernel_print("    `-- \033[38;5;183mRBP (offset 0x%lx)\033[0m\n", ctx->rbp - base + local_range->offset);
        if (cr2 >= base && cr2 < end)
            kernel_print("    `-- \033[38;5;159mCR2 (offset 0x%lx)\033[0m\n", cr2 - base + local_range->offset);
    );

    // Kill the process (mirrors syscall_exit logic)
    struct pagemap *old_pagemap = proc->pagemap;

    vmm_switch_to(vmm_kernel_pagemap);
    thread->process = kernel_process;

    for (int i = 0; i < MAX_FDS; i++) {
        fdnum_close(proc, i, true);
    }

    if (proc->pid != -1) {
        struct process *pid1 = sched_get_process(1);

        VECTOR_FOR_EACH(&proc->children, it,
            VECTOR_PUSH_BACK(&pid1->children, *it);
            VECTOR_PUSH_BACK(&pid1->child_events, &(*it)->event);
        );
    }

    vmm_destroy_pagemap(old_pagemap);

    proc->status = W_EXITCODE(0, signal);

    event_trigger(&proc->event, false);
    sched_dequeue_and_die();
}

static void exception_handler(uint8_t vector, struct cpu_ctx *ctx) {
    if (vector == 0xe && mmap_handle_pf(ctx)) {
        return;
    }

    if (ctx->cs == 0x4b) {
        userspace_crash(vector, ctx);
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
