#ifndef LIB__LOCK_K_H_
#define LIB__LOCK_K_H_

#include <stdbool.h>
#include <lib/misc.k.h>

typedef struct {
    int lock;
    void *last_acquirer;
} spinlock_t;

#define SPINLOCK_INIT {0, NULL}

static inline bool spinlock_test_and_acq(spinlock_t *lock) {
    return CAS(&lock->lock, 0, 1);
}

void spinlock_acquire(spinlock_t *lock);
void spinlock_acquire_no_dead_check(spinlock_t *lock);

static inline void spinlock_release(spinlock_t *lock) {
    lock->last_acquirer = NULL;
    CAS(&lock->lock, 1, 0);
}

#endif
