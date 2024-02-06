#include <stdbool.h>
#include <stdnoreturn.h>
#include <lib/errno.k.h>
#include <lib/print.k.h>
#include <lib/misc.k.h>
#include <lib/alloc.k.h>
#include <lib/vector.k.h>
#include <lib/resource.k.h>
#include <lib/debug.k.h>
#include <sched/sched.k.h>
#include <dev/lapic.k.h>
#include <sys/cpu.k.h>
#include <sys/idt.k.h>
#include <mm/mmap.k.h>
#include <mm/pmm.k.h>
#include <mm/vmm.k.h>
#include <fs/vfs/vfs.k.h>
#include <sys/wait.h>

struct process *kernel_process;

static struct thread *running_queue[MAX_RUNNING_THREADS];

static uint8_t sched_vector;

static void sched_entry(int vector, struct cpu_ctx *ctx);

void sched_init(void) {
    sched_vector = idt_allocate_vector();
    kernel_print("sched: Scheduler interrupt vector is 0x%x\n", sched_vector);

    isr[sched_vector] = sched_entry;
    idt_set_ist(sched_vector, 1);

    kernel_process = sched_new_process(NULL, vmm_kernel_pagemap);
}

static struct thread *get_next_thread(void) {
    struct cpu_local *cpu = this_cpu();

    int orig_i = cpu->last_run_queue_index;

    if (orig_i >= MAX_RUNNING_THREADS) {
        orig_i = 0;
    }

    int index = orig_i + 1;

    for (;;) {
        if (index >= MAX_RUNNING_THREADS) {
            index = 0;
        }

        struct thread *thread = running_queue[index];

        if (thread != NULL) {
            if (spinlock_test_and_acq(&thread->lock) == true) {
                cpu->last_run_queue_index = index;
                return thread;
            }
        }

        if (index == orig_i) {
            break;
        }

        index++;
    }

    cpu->last_run_queue_index = index;
    return NULL;
}

#if defined (__x86_64__)

static noreturn void thread_spinup(struct cpu_ctx *ctx) {
    asm volatile (
        "mov %0, %%rsp\n\t"
        "pop %%rax\n\t"
        "mov %%eax, %%ds\n\t"
        "pop %%rax\n\t"
        "mov %%eax, %%es\n\t"
        "pop %%rax\n\t"
        "pop %%rbx\n\t"
        "pop %%rcx\n\t"
        "pop %%rdx\n\t"
        "pop %%rsi\n\t"
        "pop %%rdi\n\t"
        "pop %%rbp\n\t"
        "pop %%r8\n\t"
        "pop %%r9\n\t"
        "pop %%r10\n\t"
        "pop %%r11\n\t"
        "pop %%r12\n\t"
        "pop %%r13\n\t"
        "pop %%r14\n\t"
        "pop %%r15\n\t"
        "add $8, %%rsp\n\t"
        "swapgs\n\t"
        "iretq\n\t"
        :
        : "rm" (ctx)
        : "memory"
    );
    __builtin_unreachable();
}

#endif

static void sched_entry(int vector, struct cpu_ctx *ctx) {
    (void)vector;

    lapic_timer_stop();

    struct cpu_local *cpu = this_cpu();

    cpu->active = true;

    struct thread *current_thread = sched_current_thread();

    struct thread *next_thread = get_next_thread();

    if (current_thread != cpu->idle_thread) {
        spinlock_release(&current_thread->yield_await);

        if (next_thread == NULL && current_thread->enqueued) {
            lapic_eoi();
            lapic_timer_oneshot(current_thread->timeslice, sched_vector);
            return;
        }

        current_thread->ctx = *ctx;

#if defined (__x86_64__)
        current_thread->gs_base = get_kernel_gs_base();
        current_thread->fs_base = get_fs_base();
        current_thread->cr3 = read_cr3();
        fpu_save(current_thread->fpu_storage);
#endif

        current_thread->running_on = -1;
        spinlock_release(&current_thread->lock);
    }

    if (next_thread == NULL) {
        lapic_eoi();
#if defined (__x86_64__)
        set_gs_base(cpu->idle_thread);
        set_kernel_gs_base(cpu->idle_thread);
#endif
        cpu->active = false;
        vmm_switch_to(vmm_kernel_pagemap);
        sched_await();
    }

    current_thread = next_thread;

#if defined (__x86_64__)
    set_gs_base(current_thread);
    if (current_thread->ctx.cs == 0x4b) {
        set_kernel_gs_base(current_thread->gs_base);
    } else {
        set_kernel_gs_base(current_thread);
    }
    set_fs_base(current_thread->fs_base);

    if (sysenter) {
        wrmsr(0x175, (uint64_t)current_thread->kernel_stack);
    } else {
        cpu->tss.ist3 = (uint64_t)current_thread->kernel_stack;
    }

    cpu->tss.ist2 = (uint64_t)current_thread->pf_stack;

    if (read_cr3() != current_thread->cr3) {
        write_cr3(current_thread->cr3);
    }

    fpu_restore(current_thread->fpu_storage);
#endif

    current_thread->running_on = cpu->cpu_number;
    current_thread->this_cpu = cpu;

    lapic_eoi();
    lapic_timer_oneshot(current_thread->timeslice, sched_vector);

    struct cpu_ctx *new_ctx = &current_thread->ctx;

    thread_spinup(new_ctx);
}

noreturn void sched_await(void) {
    interrupt_toggle(false);
    lapic_timer_oneshot(20000, sched_vector);
    interrupt_toggle(true);
    for (;;) {
        halt();
    }
    __builtin_unreachable();
}

void sched_yield(bool save_ctx) {
    interrupt_toggle(false);

    lapic_timer_stop();

    struct thread *thread = sched_current_thread();

    struct cpu_local *cpu = this_cpu();

    if (save_ctx) {
        spinlock_acquire(&thread->yield_await);
    } else {
        set_gs_base(cpu->idle_thread);
        set_kernel_gs_base(cpu->idle_thread);
    }

    lapic_send_ipi(cpu->lapic_id, sched_vector);

    interrupt_toggle(true);

    if (save_ctx) {
        spinlock_acquire(&thread->yield_await);
        spinlock_release(&thread->yield_await);
    } else {
        for (;;) {
            halt();
        }
    }
}

bool sched_enqueue_thread(struct thread *thread, bool by_signal) {
    if (thread->enqueued == true) {
        return true;
    }

    thread->enqueued_by_signal = by_signal;

    for (size_t i = 0; i < MAX_RUNNING_THREADS; i++) {
        if (CAS(&running_queue[i], NULL, thread)) {
            thread->enqueued = true;

            for (size_t j = 0; j < cpu_count; j++) {
                if (cpus[j].active == false) {
                    lapic_send_ipi(cpus[j].lapic_id, sched_vector);
                    break;
                }
            }

            return true;
        }
    }

    return false;
}

bool sched_dequeue_thread(struct thread *thread) {
    if (!thread->enqueued) {
        return true;
    }

    for (size_t i = 0; i < MAX_RUNNING_THREADS; i++) {
        if (CAS(&running_queue[i], thread, NULL)) {
            thread->enqueued = false;

            return true;
        }
    }

    return false;
}

noreturn void sched_dequeue_and_die(void) {
    interrupt_toggle(false);

    struct thread *thread = sched_current_thread();

    sched_dequeue_thread(thread);

    // TODO: Free stacks

    sched_yield(false);
    __builtin_unreachable();
}

static VECTOR_TYPE(struct process *) processes = VECTOR_INIT;

struct process *sched_new_process(struct process *old_proc, struct pagemap *pagemap) {
    struct process *new_proc = ALLOC(struct process);
    if (new_proc == NULL) {
        errno = ENOMEM;
        goto cleanup;
    }

    new_proc->threads = (typeof(new_proc->threads))VECTOR_INIT;

    if (old_proc != NULL) {
        memcpy(new_proc->name, old_proc->name, sizeof(old_proc->name));

        new_proc->pagemap = vmm_fork_pagemap(old_proc->pagemap);
        if (new_proc->pagemap == NULL) {
            goto cleanup;
        }

        new_proc->ppid = old_proc->pid;
        new_proc->thread_stack_top = old_proc->thread_stack_top;
        new_proc->mmap_anon_base = old_proc->mmap_anon_base;
        new_proc->cwd = old_proc->cwd;
        new_proc->umask = old_proc->umask;
    } else {
        new_proc->ppid = 0;
        new_proc->pagemap = pagemap;
        new_proc->thread_stack_top = 0x70000000000;
        new_proc->mmap_anon_base = 0x80000000000;
        new_proc->cwd = vfs_root;
        new_proc->umask = S_IWGRP | S_IWOTH;
    }

    new_proc->pid = VECTOR_PUSH_BACK(&processes, new_proc);

    if (old_proc != NULL) {
        VECTOR_PUSH_BACK(&old_proc->children, new_proc);
        VECTOR_PUSH_BACK(&old_proc->child_events, &new_proc->event);
    }

    return new_proc;

cleanup:
    if (new_proc != NULL) {
        free(new_proc);
    }
    return NULL;
}

#define STACK_SIZE 0x40000

struct thread *sched_new_kernel_thread(void *pc, void *arg, bool enqueue) {
    struct thread *thread = ALLOC(struct thread);

    thread->lock = (spinlock_t)SPINLOCK_INIT;
    thread->yield_await = (spinlock_t)SPINLOCK_INIT;
    thread->stacks = (typeof(thread->stacks))VECTOR_INIT;

    void *stack_phys = pmm_alloc(STACK_SIZE / PAGE_SIZE);
    VECTOR_PUSH_BACK(&thread->stacks, stack_phys);
    void *stack = stack_phys + STACK_SIZE + VMM_HIGHER_HALF;

#if defined (__x86_64__)
    thread->ctx.cs = 0x28;
    thread->ctx.ds = thread->ctx.es = thread->ctx.ss = 0x30;
    thread->ctx.rflags = 0x202;
    thread->ctx.rip = (uint64_t)pc;
    thread->ctx.rdi = (uint64_t)arg;
    thread->ctx.rsp = (uint64_t)stack;

    thread->cr3 = (uint64_t)kernel_process->pagemap->top_level - VMM_HIGHER_HALF;
    thread->gs_base = thread;
#endif

    thread->process = kernel_process;
    thread->timeslice = 5000;
    thread->running_on = -1;
    thread->fpu_storage = pmm_alloc(DIV_ROUNDUP(fpu_storage_size, PAGE_SIZE))
                        + VMM_HIGHER_HALF;
    thread->self = thread;

    if (enqueue) {
        sched_enqueue_thread(thread, false);
    }

    return thread;
}

int syscall_new_thread(void *_, void *entry, void *stack) {
    (void)_;

    DEBUG_SYSCALL_ENTER("new_thread(%lx, %lx)", entry, stack);

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct thread *new = sched_new_user_thread(proc, entry, NULL, stack, NULL, NULL, NULL, true);

    int tid = new->tid;

    DEBUG_SYSCALL_LEAVE("%d", tid);

    return tid;
}

noreturn int syscall_exit_thread(void *_) {
    (void)_;

    DEBUG_SYSCALL_ENTER("exit_thread()");

    sched_dequeue_and_die();
}

struct thread *sched_new_user_thread(struct process *proc, void *pc, void *arg, void *sp,
                                     const char **argv, const char **envp, struct auxval *auxval, bool enqueue) {
    struct thread *thread = ALLOC(struct thread);
    if (thread == NULL) {
        errno = ENOMEM;
        goto fail;
    }

    thread->lock = (spinlock_t)SPINLOCK_INIT;
    thread->yield_await = (spinlock_t)SPINLOCK_INIT;
    thread->enqueued = false;
    thread->stacks = (typeof(thread->stacks))VECTOR_INIT;

    uintptr_t *stack, *stack_vma;
    if (sp == NULL) {
        void *stack_phys = pmm_alloc(STACK_SIZE / PAGE_SIZE);
        if (stack_phys == NULL) {
            errno = ENOMEM;
            goto fail;
        }

        stack = stack_phys + STACK_SIZE + VMM_HIGHER_HALF;
        stack_vma = (void *)proc->thread_stack_top;
        if (!mmap_range(proc->pagemap, proc->thread_stack_top - STACK_SIZE, (uintptr_t)stack_phys,
                        STACK_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS)) {
            pmm_free(stack_phys, STACK_SIZE / PAGE_SIZE);
            goto fail;
        }

        proc->thread_stack_top -= STACK_SIZE - PAGE_SIZE;
    } else {
        stack = sp;
        stack_vma = sp;
    }

    void *kernel_stack_phys = pmm_alloc(STACK_SIZE / PAGE_SIZE);
    VECTOR_PUSH_BACK(&thread->stacks, kernel_stack_phys);
    thread->kernel_stack = kernel_stack_phys + STACK_SIZE + VMM_HIGHER_HALF;

    void *pf_stack_phys = pmm_alloc(STACK_SIZE / PAGE_SIZE);
    VECTOR_PUSH_BACK(&thread->stacks, pf_stack_phys);
    thread->pf_stack = pf_stack_phys + STACK_SIZE + VMM_HIGHER_HALF;

#if defined (__x86_64__)
    thread->ctx.cs = 0x4b;
    thread->ctx.ds = thread->ctx.es = thread->ctx.ss = 0x53;
    thread->ctx.rflags = 0x202;
    thread->ctx.rip = (uint64_t)pc;
    thread->ctx.rdi = (uint64_t)arg;
    thread->ctx.rsp = (uint64_t)stack_vma;
    thread->cr3 = (uint64_t)proc->pagemap->top_level - VMM_HIGHER_HALF;
#endif

    thread->self = thread;
    thread->process = proc;
    thread->timeslice = 5000;
    thread->running_on = -1;
    thread->fpu_storage = pmm_alloc(DIV_ROUNDUP(fpu_storage_size, PAGE_SIZE))
                          + VMM_HIGHER_HALF;

    // Set up FPU control word and MXCSR as defined in the sysv ABI
    fpu_restore(thread->fpu_storage);
    uint16_t default_fcw = 0b1100111111;
    asm volatile ("fldcw %0" :: "m"(default_fcw) : "memory");
    uint32_t default_mxcsr = 0b1111110000000;
    asm volatile ("ldmxcsr %0" :: "m"(default_mxcsr) : "memory");
    fpu_save(thread->fpu_storage);

    thread->tid = proc->threads.length;

    if (proc->threads.length == 0) {
        void *stack_top = stack;

        int envp_len;
        for (envp_len = 0; envp[envp_len] != NULL; envp_len++) {
            size_t length = strlen(envp[envp_len]);
            stack = (void *)stack - length - 1;
            memcpy(stack, envp[envp_len], length);
        }

        int argv_len;
        for (argv_len = 0; argv[argv_len] != NULL; argv_len++) {
            size_t length = strlen(argv[argv_len]);
            stack = (void *)stack - length - 1;
            memcpy(stack, argv[argv_len], length);
        }

        stack = (uintptr_t *)ALIGN_DOWN((uintptr_t)stack, 16);
        if (((argv_len + envp_len + 1) & 1) != 0) {
            stack--;
        }

        // Auxilary vector
        *(--stack) = 0, *(--stack) = 0;
        stack -= 2; stack[0] = AT_SECURE, stack[1] = 0;
        stack -= 2; stack[0] = AT_ENTRY, stack[1] = auxval->at_entry;
        stack -= 2; stack[0] = AT_PHDR,  stack[1] = auxval->at_phdr;
        stack -= 2; stack[0] = AT_PHENT, stack[1] = auxval->at_phent;
        stack -= 2; stack[0] = AT_PHNUM, stack[1] = auxval->at_phnum;

        uintptr_t old_rsp = thread->ctx.rsp;

        // Environment variables
        *(--stack) = 0;
        stack -= envp_len;
        for (int i = 0; i < envp_len; i++) {
            old_rsp -= strlen(envp[i]) + 1;
            stack[i] = old_rsp;
        }

        // Arguments
        *(--stack) = 0;
        stack -= argv_len;
        for (int i = 0; i < argv_len; i++) {
            old_rsp -= strlen(argv[i]) + 1;
            stack[i] = old_rsp;
        }

        *(--stack) = argv_len;

        thread->ctx.rsp -= stack_top - (void *)stack;
    }

    VECTOR_PUSH_BACK(&proc->threads, thread);

    if (enqueue) {
        sched_enqueue_thread(thread, false);
    }

    return thread;

fail:
    if (thread != NULL) {
        free(thread);
    }
    return NULL;
}

void syscall_set_fs_base(void *_, void *base) {
    (void)_;

    DEBUG_SYSCALL_ENTER("set_fs_base(%lx)", base);

    set_fs_base(base);

    DEBUG_SYSCALL_LEAVE("");
}

void syscall_set_gs_base(void *_, void *base) {
    (void)_;

    DEBUG_SYSCALL_ENTER("set_gs_base(%lx)", base);

    set_gs_base(base);

    DEBUG_SYSCALL_LEAVE("");
}

pid_t syscall_getpid(void *_) {
    (void)_;

    DEBUG_SYSCALL_ENTER("getpid()");

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    int ret = proc->pid;

    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_fork(struct cpu_ctx *ctx) {
    DEBUG_SYSCALL_ENTER("fork()");

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;
    struct process *new_proc = sched_new_process(proc, NULL);

    for (int i = 0; i < MAX_FDS; i++) {
        if (proc->fds[i] == NULL) {
            continue;
        }

        if (fdnum_dup(proc, i, new_proc, i, 0, true, false) != i) {
            goto fail;
        }
    }

    struct thread *new_thread = ALLOC(struct thread);
    if (new_thread == NULL) {
        errno = ENOMEM;
        goto fail;
    }

    new_thread->lock = (spinlock_t)SPINLOCK_INIT;
    new_thread->yield_await = (spinlock_t)SPINLOCK_INIT;
    new_thread->enqueued = false;
    new_thread->stacks = (typeof(new_thread->stacks))VECTOR_INIT;

    void *kernel_stack_phys = pmm_alloc(STACK_SIZE / PAGE_SIZE);
    VECTOR_PUSH_BACK(&new_thread->stacks, kernel_stack_phys);
    new_thread->kernel_stack = kernel_stack_phys + STACK_SIZE + VMM_HIGHER_HALF;

    void *pf_stack_phys = pmm_alloc(STACK_SIZE / PAGE_SIZE);
    VECTOR_PUSH_BACK(&new_thread->stacks, pf_stack_phys);
    new_thread->pf_stack = kernel_stack_phys + STACK_SIZE + VMM_HIGHER_HALF;

    new_thread->ctx = *ctx;

#if defined (__x86_64__)
    new_thread->cr3 = (uint64_t)new_proc->pagemap->top_level - VMM_HIGHER_HALF;
#endif

    new_thread->self = new_thread;
    new_thread->process = new_proc;
    new_thread->timeslice = thread->timeslice;
    new_thread->gs_base = get_kernel_gs_base();
    new_thread->fs_base = get_fs_base();
    new_thread->running_on = -1;
    new_thread->fpu_storage = pmm_alloc(DIV_ROUNDUP(fpu_storage_size, PAGE_SIZE))
                              + VMM_HIGHER_HALF;

    memcpy(new_thread->fpu_storage, thread->fpu_storage, fpu_storage_size);

    new_thread->ctx.rax = 0;
    new_thread->ctx.rbx = 0;

    VECTOR_PUSH_BACK(&new_proc->threads, new_thread);

    sched_enqueue_thread(new_thread, false);

    ret = new_proc->pid;
    goto cleanup;

fail:
    // TODO: Properly clean up
    free(new_proc);

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_exec(void *_, const char *path, const char **argv, const char **envp) {
    (void)_;

    DEBUG_SYSCALL_ENTER("exec(%s, %lx, %lx)", path, argv, envp);

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct pagemap *new_pagemap = vmm_new_pagemap();
    struct auxval auxv, ld_auxv;
    const char *ld_path;

    struct vfs_node *node = vfs_get_node(proc->cwd, path, true);
    if (node == NULL || !elf_load(new_pagemap, node->resource, 0x0, &auxv, &ld_path)) {
        goto fail;
    }

    struct vfs_node *ld_node = vfs_get_node(vfs_root, ld_path, true);
    if (ld_node == NULL || !elf_load(new_pagemap, ld_node->resource, 0x40000000, &ld_auxv, NULL)) {
        goto fail;
    }

    struct pagemap *old_pagemap = proc->pagemap;

    proc->pagemap = new_pagemap;
    proc->thread_stack_top = 0x70000000000;
    proc->mmap_anon_base = 0x80000000000;

    // TODO: Kill old threads
    proc->threads = (typeof(proc->threads))VECTOR_INIT;

    uint64_t entry = ld_path == NULL ? auxv.at_entry : ld_auxv.at_entry;

    struct thread *new_thread = sched_new_user_thread(proc, (void *)entry, NULL, NULL, argv, envp, &auxv, true);

    if (new_thread == NULL) {
        goto fail;
    }

    vfs_pathname(node, proc->name, sizeof(proc->name) - 1);
    vmm_switch_to(vmm_kernel_pagemap);

    thread->process = kernel_process;

    vmm_destroy_pagemap(old_pagemap);
    sched_dequeue_and_die();

fail:
    DEBUG_SYSCALL_LEAVE("%d", -1);
    return -1;
}

int syscall_exit(void *_, int status) {
    (void)_;

    DEBUG_SYSCALL_ENTER("exit(%d)", status);

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct pagemap *old_pagemap = proc->pagemap;

    vmm_switch_to(vmm_kernel_pagemap);
    thread->process = kernel_process;

    for (int i = 0; i < MAX_FDS; i++) {
        fdnum_close(proc, i, true);
    }

    if (proc->pid != -1) {
        struct process *pid1 = VECTOR_ITEM(&processes, 1);

        VECTOR_FOR_EACH(&proc->children, it,
            VECTOR_PUSH_BACK(&pid1->children, *it);
            VECTOR_PUSH_BACK(&pid1->child_events, &(*it)->event);
        );
    }

    vmm_destroy_pagemap(old_pagemap);

    proc->status = W_EXITCODE(status, 0);

    event_trigger(&proc->event, false);
    sched_dequeue_and_die();

    // TODO: Kill all threads too
}

pid_t syscall_waitpid(void *_, int pid, int *status, int flags) {
    (void)_;

    DEBUG_SYSCALL_ENTER("waitpid(%d, %lx, %x)", pid, status, flags);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    struct process *child = NULL;
    struct event *child_event = NULL;
    struct event **events = NULL;
    size_t event_num = 0;

    if (pid == -1) {
        if (proc->children.length == 0) {
            errno = ECHILD;
            goto cleanup;
        }

        events = proc->child_events.data;
        event_num = proc->child_events.length;
    } else if (pid < -1 || pid == 0) {
        errno = EINVAL;
        goto cleanup;
    } else {
        if (proc->children.length == 0) {
            errno = ECHILD;
            goto cleanup;
        }

        child = VECTOR_ITEM(&processes, pid);

        if ((ssize_t)child == VECTOR_INVALID_INDEX || child->ppid != proc->pid) {
            errno = ECHILD;
            goto cleanup;
        }

        child_event = &child->event;
        events = &child_event;
        event_num = 1;
    }

    bool block = (flags & WNOHANG) == 0;
    ssize_t which = event_await(events, event_num, block);
    if (which == -1) {
        if (block) {
            ret = 0;
            goto cleanup;
        } else {
            errno = EINTR;
            goto cleanup;
        }
    }

    if (child == NULL) {
        child = VECTOR_ITEM(&proc->children, which);
    }

    *status = child->status;

    VECTOR_REMOVE_BY_VALUE(&proc->child_events, &child->event);
    VECTOR_REMOVE_BY_VALUE(&proc->children, child);

    VECTOR_REMOVE_BY_VALUE(&processes, child);

    ret = child->pid;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}
