#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/cpu.k.h>
#include <sys/gdt.k.h>
#include <sys/idt.k.h>
#include <sys/syscall.k.h>
#include <dev/lapic.k.h>
#include <mm/pmm.k.h>
#include <mm/vmm.k.h>
#include <lib/print.k.h>
#include <lib/panic.k.h>
#include <lib/misc.k.h>
#include <lib/lock.k.h>
#include <sched/sched.k.h>
#include <limine.h>

bool sysenter = false;

size_t fpu_storage_size = 0;
void (*fpu_save)(void *ctx) = NULL;
void (*fpu_restore)(void *ctx) = NULL;

size_t cpu_count = 0;

#define CPU_STACK_SIZE 0x10000

static size_t cpus_started_i = 0;

static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0
};

static void single_cpu_init(struct limine_smp_info *smp_info) {
    struct cpu_local *cpu_local = (void *)smp_info->extra_argument;
    int cpu_number = cpu_local->cpu_number;

    cpu_local->lapic_id = smp_info->lapic_id;

    gdt_reload();
    idt_reload();

    gdt_load_tss(&cpu_local->tss);

    vmm_switch_to(vmm_kernel_pagemap);

    struct thread *idle_thread = ALLOC(struct thread);

    idle_thread->self = idle_thread;
    idle_thread->this_cpu = cpu_local;
    idle_thread->process = kernel_process;

    cpu_local->idle_thread = idle_thread;

    set_gs_base(idle_thread);

    uint64_t *common_int_stack_phys = pmm_alloc(CPU_STACK_SIZE / PAGE_SIZE);
    if (common_int_stack_phys == NULL) {
        panic(NULL, true, "Allocation failure");
    }
    uint64_t *common_int_stack =
        (void *)common_int_stack_phys + CPU_STACK_SIZE + VMM_HIGHER_HALF;
    cpu_local->tss.rsp0 = (uint64_t)common_int_stack;

    uint64_t *sched_stack_phys = pmm_alloc(CPU_STACK_SIZE / PAGE_SIZE);
    if (sched_stack_phys == NULL) {
        panic(NULL, true, "Allocation failure");
    }
    uint64_t *sched_stack =
        (void *)sched_stack_phys + CPU_STACK_SIZE + VMM_HIGHER_HALF;
    cpu_local->tss.ist1 = (uint64_t)sched_stack;

    // Enable PAT (write-combining/write-protect)
    uint64_t pat = rdmsr(0x277);
    pat &= 0xffffffff;
    pat |= (uint64_t)0x0105 << 32;
    wrmsr(0x277, pat);

    // Enable SSE/SSE2
    uint64_t cr0 = read_cr0();
    cr0 &= ~((uint64_t)1 << 2);
    cr0 |= (uint64_t)1 << 1;
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= (uint64_t)3 << 9;
    write_cr4(cr4);

    uint32_t eax, ebx, ecx, edx;

    if (sysenter) {
        if (cpu_local->bsp) {
            kernel_print("cpu: Using SYSENTER\n");
        }

        wrmsr(0x174, 0x28); // CS
        wrmsr(0x176, (uint64_t)syscall_sysenter_entry);
    } else {
        if (cpu_local->bsp) {
            kernel_print("cpu: SYSENTER not present! Using #UD\n");

            idt_register_handler(0x06, syscall_ud_entry, 0x8e);
            idt_set_ist(0x6, 3); // #UD uses IST 3
        }
    }

    if (cpuid(1, 0, &eax, &ebx, &ecx, &edx) && (ecx & CPUID_XSAVE)) {
        if (cpu_local->bsp) {
            kernel_print("fpu: xsave supported\n");
        }

        // Enable XSAVE and x{get,set}bv
        cr4 = read_cr4();
        cr4 |= (uint64_t)1 << 18;
        write_cr4(cr4);

        uint64_t xcr0 = 0;
        if (cpu_local->bsp) {
            kernel_print("fpu: Saving x87 state using xsave\n");
        }
        xcr0 |= (uint64_t)1 << 0;
        if (cpu_local->bsp) {
            kernel_print("fpu: Saving SSE state using xsave\n");
        }
        xcr0 |= (uint64_t)1 << 1;

        if (ecx & CPUID_AVX) {
            if (cpu_local->bsp) {
                kernel_print("fpu: Saving AVX state using xsave\n");
            }
            xcr0 |= (uint64_t)1 << 2;
        }

        if (cpuid(7, 0, &eax, &ebx, &ecx, &edx) && (ebx & CPUID_AVX512)) {
            if (cpu_local->bsp) {
                kernel_print("fpu: Saving AVX-512 state using xsave\n");
            }
            xcr0 |= (uint64_t)1 << 5;
            xcr0 |= (uint64_t)1 << 6;
            xcr0 |= (uint64_t)1 << 7;
        }

        wrxcr(0, xcr0);

        if (!cpuid(0xd, 0, &eax, &ebx, &ecx, &edx)) {
            panic(NULL, true, "CPUID failure");
        }

        fpu_storage_size = ecx;
        fpu_save = xsave;
        fpu_restore = xrstor;
    } else {
        if (cpu_local->bsp) {
            kernel_print("fpu: Using legacy fxsave\n");
        }
        fpu_storage_size = 512;
        fpu_save = fxsave;
        fpu_restore = fxrstor;
    }

    static spinlock_t lock = SPINLOCK_INIT;
    spinlock_acquire(&lock);
    lapic_init();
    spinlock_release(&lock);

    interrupt_toggle(true);

    kernel_print("cpu: Processor #%u online!\n", cpu_number);

    cpus_started_i++;

    if (!cpu_local->bsp) {
        sched_await();
    }
}

uint32_t bsp_lapic_id;
bool smp_started = false;

static void sysenter_check_exception(uint8_t vector, struct cpu_ctx *ctx) {
    // If this was a #GP, we have sysenter
    if (vector == 0x0d) {
        sysenter = true;
    }

    // Skip over test sysenter instruction
    ctx->rip += 2;
}

struct cpu_local *cpus = NULL;

void cpu_init(void) {
    uint32_t eax, ebx, ecx, edx;

    bool is_amd = cpuid(0, 0, &eax, &ebx, &ecx, &edx)
               && ebx == 0x68747541
               && edx == 0x69746e65
               && ecx == 0x444d4163;

    if (!is_amd && cpuid(1, 0, &eax, &ebx, &ecx, &edx) && (edx & CPUID_SEP)) {
        uint8_t old_ud_ist = idt_get_ist(0x06);
        uint8_t old_gp_ist = idt_get_ist(0x0d);
        void *old_ud_handler = isr[0x06];
        void *old_gp_handler = isr[0x0d];

        idt_set_ist(0x06, 0);
        idt_set_ist(0x0d, 0);
        isr[0x06] = sysenter_check_exception;
        isr[0x0d] = sysenter_check_exception;

        wrmsr(0x174, 0); // CS == NULL

        asm volatile (
            "sysenter"
            :
            :
            : "memory"
        );

        idt_set_ist(0x06, old_ud_ist);
        idt_set_ist(0x0d, old_gp_ist);
        isr[0x06] = old_ud_handler;
        isr[0x0d] = old_gp_handler;
    }

    struct limine_smp_response *smp_resp = smp_request.response;

    ASSERT(smp_resp != NULL);

    kernel_print("cpu: %u processors detected\n", smp_resp->cpu_count);

    cpu_count = smp_resp->cpu_count;

    cpus = alloc(cpu_count * sizeof(struct cpu_local));

    bsp_lapic_id = smp_resp->bsp_lapic_id;

    for (size_t i = 0; i < cpu_count; i++) {
        struct limine_smp_info *cpu = smp_resp->cpus[i];

        struct cpu_local *cpu_local = &cpus[i];
        cpu->extra_argument = (uint64_t)cpu_local;
        cpu_local->cpu_number = i;

        if (cpu->lapic_id != smp_resp->bsp_lapic_id) {
            cpu->goto_address = single_cpu_init;
        } else {
            cpu_local->bsp = true;
            single_cpu_init(cpu);
        }
    }

    while (cpus_started_i != smp_resp->cpu_count) {
        asm ("pause");
    }

    smp_started = true;
}

struct cpu_local *this_cpu(void) {
    if (interrupt_state()) {
        panic(NULL, true, "Calling this_cpu() with interrupts on is a bug");
    }

    return sched_current_thread()->this_cpu;
}
