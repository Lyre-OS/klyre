#ifndef LIB__DEBUG_K_H_
#define LIB__DEBUG_K_H_

#include <stdint.h>
#include <lib/print.k.h>
#include <lib/errno.k.h>
#include <sched/proc.k.h>

uint64_t debug_get_syscall_id(void);
char *strerror(int err);

#define DEBUG_SYSCALL_ENTER(FMT, ...) \
    uint64_t DEBUG_syscall_id = debug_get_syscall_id(); \
    struct thread *DEBUG_thread = sched_current_thread(); \
    struct process *DEBUG_proc = DEBUG_thread->process; \
    errno = 0; \
    debug_print(DEBUG_proc->pid - 1, "\e[32m%llu\e[m - %s[%d:%d]: " FMT, DEBUG_syscall_id, DEBUG_proc->name, DEBUG_proc->pid, DEBUG_thread->tid, ## __VA_ARGS__);

#define DEBUG_SYSCALL_LEAVE(FMT, ...) \
    debug_print(DEBUG_proc->pid - 1, "\e[31m%llu\e[m - %s[%d:%d]: returning " FMT " (%s)", DEBUG_syscall_id, DEBUG_proc->name, DEBUG_proc->pid, DEBUG_thread->tid, ## __VA_ARGS__, strerror(errno));

#endif
