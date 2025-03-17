#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <lib/alloc.k.h>
#include <lib/errno.k.h>
#include <lib/lock.k.h>
#include <lib/misc.k.h>
#include <lib/panic.k.h>
#include <lib/print.k.h>
#include <lib/vector.k.h>
#include <lib/debug.k.h>
#include <time/time.k.h>
#include <dev/pit.k.h>
#include <sched/sched.k.h>
#include <time.h>

static volatile struct limine_boot_time_request boot_time_request = {
    .id = LIMINE_BOOT_TIME_REQUEST,
    .revision = 0
};

struct timespec time_monotonic = {0, 0};
struct timespec time_realtime = {0, 0};

static spinlock_t timers_lock = SPINLOCK_INIT;
static VECTOR_TYPE(struct timer *) armed_timers = VECTOR_INIT;

struct timer *timer_new(struct timespec when) {
    struct timer *timer = ALLOC(struct timer);
    if (timer == NULL) {
        return NULL;
    }

    timer->when = when;
    timer->fired = false;
    timer->index = -1;

    timer_arm(timer);
    return timer;
}

void timer_arm(struct timer *timer) {
    spinlock_acquire(&timers_lock);

    timer->index = armed_timers.length;
    timer->fired = false;

    VECTOR_PUSH_BACK(&armed_timers, timer);
    spinlock_release(&timers_lock);
}

void timer_disarm(struct timer *timer) {
    spinlock_acquire(&timers_lock);

    if (armed_timers.length == 0 || timer->index == -1 || (size_t)timer->index >= armed_timers.length) {
        goto cleanup;
    }

    armed_timers.data[timer->index] = VECTOR_ITEM(&armed_timers, armed_timers.length - 1);
    VECTOR_ITEM(&armed_timers, timer->index)->index = timer->index;
    VECTOR_REMOVE(&armed_timers, armed_timers.length - 1);

    timer->index = -1;

cleanup:
    spinlock_release(&timers_lock);
}

void time_init(void) {
    struct limine_boot_time_response *boot_time_resp = boot_time_request.response;

    time_realtime.tv_sec = boot_time_resp->boot_time;

    pit_init();
}

void timer_handler(void) {
    struct timespec interval = {
        .tv_sec = 0,
        .tv_nsec = 1000000000 / TIMER_FREQ
    };

    time_monotonic = timespec_add(time_monotonic, interval);
    time_realtime = timespec_add(time_realtime, interval);

    if (spinlock_test_and_acq(&timers_lock)) {
        for (size_t i = 0; i < armed_timers.length; i++) {
            struct timer *timer = VECTOR_ITEM(&armed_timers, i);
            if (timer->fired) {
                continue;
            }

            timer->when = timespec_sub(timer->when, interval);
            if (timer->when.tv_sec == 0 && timer->when.tv_nsec == 0) {
                event_trigger(&timer->event, false);
                timer->fired = true;
            }
        }

        spinlock_release(&timers_lock);
    }
}

void time_nsleep(uint64_t ns) {
    struct timespec duration = { .tv_sec = ns / 1000000000, .tv_nsec = ns };
    struct timer *timer = NULL;

    timer = timer_new(duration);
    if (timer == NULL) {
        goto cleanup;
    }

    struct event *events[] = { &timer->event };
    event_await(events, 1, true);

    timer_disarm(timer);

    free(timer);
cleanup:
    return;
}

int syscall_sleep(void *_, struct timespec *duration, struct timespec *remaining) {
    (void)_;

    DEBUG_SYSCALL_ENTER("sleep(%lx, %lx)", duration, remaining);

    int ret = -1;

    if (duration->tv_sec == 0 && duration->tv_nsec == 0) {
        ret = 0;
        goto cleanup;
    }

    if (duration->tv_nsec < 0 || duration->tv_nsec < 0 || duration->tv_nsec > 1000000000) {
        errno = EINVAL;
        goto cleanup;
    }

    struct timer *timer = timer_new(*duration);
    if (timer == NULL) {
        goto cleanup;
    }

    struct event *event = &timer->event;

    ssize_t which = event_await(&event, 1, true);

    if (which == -1) {
        if (remaining != NULL) {
            *remaining = timer->when;
        }

        errno = EINTR;
        timer_disarm(timer);
        free(timer);
        goto cleanup;
    }

    timer_disarm(timer);
    free(timer);
    ret = 0;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}

int syscall_getclock(void *_, int which, struct timespec *out) {
    (void)_;

    DEBUG_SYSCALL_ENTER("getclock(%d, %lx)", which, out);

    int ret = -1;

    switch (which) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE:
            *out = time_realtime;
            ret = 0;
            goto cleanup;
        case CLOCK_BOOTTIME:
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_MONOTONIC_COARSE:
            *out = time_monotonic;
            ret = 0;
            goto cleanup;
        case CLOCK_PROCESS_CPUTIME_ID:
        case CLOCK_THREAD_CPUTIME_ID:
            *out = (struct timespec){.tv_sec = 0, .tv_nsec = 0};
            ret = 0;
            goto cleanup;
    }

    errno = EINVAL;

cleanup:
    DEBUG_SYSCALL_LEAVE("%d", ret);
    return ret;
}
