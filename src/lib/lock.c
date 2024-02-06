#include <lib/lock.k.h>
#include <lib/panic.k.h>

__attribute__((noinline)) void spinlock_acquire(spinlock_t *lock) {
    volatile size_t deadlock_counter = 0;
    for (;;) {
        if (spinlock_test_and_acq(lock)) {
            break;
        }
        if (++deadlock_counter >= 100000000) {
            goto deadlock;
        }
#if defined (__x86_64__)
        asm volatile ("pause");
#endif
    }
    lock->last_acquirer = __builtin_return_address(0);
    return;

deadlock:
    panic(NULL, true, "Deadlock occurred at %llx on lock %llx whose last acquirer was %llx", __builtin_return_address(0), lock, lock->last_acquirer);
}

__attribute__((noinline)) void spinlock_acquire_no_dead_check(spinlock_t *lock) {
    for (;;) {
        if (spinlock_test_and_acq(lock)) {
            break;
        }
#if defined (__x86_64__)
        asm volatile ("pause");
#endif
    }
    lock->last_acquirer = __builtin_return_address(0);
}

