#ifndef LIB__EVENT_K_H_
#define LIB__EVENT_K_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <lib/lock.k.h>

#define EVENT_MAX_LISTENERS 32

struct event_listener {
    struct thread *thread;
    size_t which;
};

struct event {
    spinlock_t lock;
    size_t pending;
    size_t listeners_i;
    struct event_listener listeners[EVENT_MAX_LISTENERS];
};

ssize_t event_await(struct event **events, size_t num_events, bool block);
size_t event_trigger(struct event *event, bool drop);

#endif
