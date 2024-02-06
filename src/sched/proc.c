#include <stdint.h>
#include <stddef.h>
#include <lib/alloc.k.h>
#include <lib/errno.k.h>
#include <lib/hashmap.k.h>
#include <lib/print.k.h>
#include <lib/debug.k.h>
#include <mm/vmm.k.h>
#include <sched/proc.k.h>
#include <sys/utsname.h>

static spinlock_t futex_lock = SPINLOCK_INIT;
static HASHMAP_TYPE(struct event *) futex_hashmap = HASHMAP_INIT(256);

void proc_init(void) {
    uintptr_t phys = 0;
    HASHMAP_INSERT(&futex_hashmap, &phys, sizeof(phys), NULL);
}

int syscall_uname(void *_, struct utsname *buffer) {
    (void)_;

    DEBUG_SYSCALL_ENTER("uname(%lx)", buffer);

    strncpy(buffer->sysname, "Lyre", sizeof(buffer->sysname));
    strncpy(buffer->nodename, "lyre", sizeof(buffer->nodename));
    strncpy(buffer->release, "0.0.1", sizeof(buffer->release));
    strncpy(buffer->version, __DATE__ " " __TIME__, sizeof(buffer->version));

    DEBUG_SYSCALL_LEAVE("%d", 0);
    return 0;
}

int syscall_futex_wait(void *_, int *ptr, int expected) {
    (void)_;

    DEBUG_SYSCALL_ENTER("futex_wait(%lx, %d)", ptr, expected);

    int ret = -1;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    if (*ptr != expected) {
        errno = EAGAIN;
        goto cleanup;
    }

    uintptr_t phys = vmm_virt2phys(proc->pagemap, (uintptr_t)ptr);

    spinlock_acquire(&futex_lock);

    struct event *event = NULL;
    if (!HASHMAP_GET(&futex_hashmap, event, &phys, sizeof(phys))) {
        event = ALLOC(struct event);
        HASHMAP_INSERT(&futex_hashmap, &phys, sizeof(phys), event);
    }

    spinlock_release(&futex_lock);

    ssize_t which = event_await(&event, 1, true);
    if (which == -1) {
        errno = EINTR;
        goto cleanup;
    }

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_futex_wake(void *_, int *ptr) {
    (void)_;

    DEBUG_SYSCALL_ENTER("futex_wake(%lx)", ptr);

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    // Make sure the page isn't demand paged
    *(volatile int *)ptr;

    uintptr_t phys = vmm_virt2phys(proc->pagemap, (uintptr_t)ptr);

    spinlock_acquire(&futex_lock);

    struct event *event = NULL;
    if (!HASHMAP_GET(&futex_hashmap, event, &phys, sizeof(phys))) {
        goto cleanup;
    }

    event_trigger(event, false);

cleanup:
    spinlock_release(&futex_lock);

    DEBUG_SYSCALL_LEAVE("%d", 0);
    return 0;
}

mode_t syscall_umask(void *_, mode_t mask) {
    (void)_;

    DEBUG_SYSCALL_ENTER("umask(%o)", mask);

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    mode_t old_mask = proc->umask;
    proc->umask = mask;

    DEBUG_SYSCALL_LEAVE("%o", old_mask);
    return old_mask;
}
