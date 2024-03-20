#ifndef SCHED__PROC_K_H_
#define SCHED__PROC_K_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/cpu.k.h>
#include <lib/lock.k.h>
#include <lib/vector.k.h>
#include <lib/resource.k.h>
#include <lib/event.k.h>

#define MAX_FDS 256
#define MAX_EVENTS 32

struct process {
    int pid;
    int ppid;
    int status;
    struct pagemap *pagemap;
    uintptr_t mmap_anon_base;
    uintptr_t thread_stack_top;
    VECTOR_TYPE(struct thread *) threads;
    VECTOR_TYPE(struct process *) children;
    VECTOR_TYPE(struct event *) child_events;
    struct event event;
    struct vfs_node *cwd;
    spinlock_t fds_lock;
    mode_t umask;
    struct f_descriptor *fds[MAX_FDS];
    char name[128];
};

struct thread {
    /// dont move ///
    struct thread *self;
    uint64_t errno;
    /////////////////

    int tid;
    spinlock_t lock;
    struct cpu_local *this_cpu;
    int running_on;
    bool enqueued;
    bool enqueued_by_signal;
    struct process *process;
    int timeslice;
    spinlock_t yield_await;
    struct cpu_ctx ctx;
    void *gs_base;
    void *fs_base;
    uint64_t cr3;
    void *fpu_storage;
    VECTOR_TYPE(void *) stacks;
    void *pf_stack;
    void *kernel_stack;
    size_t which_event;
    size_t attached_events_i;
    struct event *attached_events[MAX_EVENTS];
};

static inline struct thread *sched_current_thread(void) {
    struct thread *ret = NULL;
    asm volatile ("mov %%gs:0x0, %0" : "=r" (ret));
    return ret;
}

void proc_init(void);

#endif
