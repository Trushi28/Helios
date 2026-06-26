/**
 * @file spinlock.h
 * @brief Helios OS — Header-only spinlock (SMP-ready).
 *
 * Phase 1 is uniprocessor but we implement a real LOCK XCHG spinlock
 * so Phase 2 SMP just works without changing any locking code.
 */

#ifndef HELIOS_SPINLOCK_H
#define HELIOS_SPINLOCK_H

#include <helios/types.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

/**
 * @brief Acquire the spinlock. Spins with PAUSE hint until acquired.
 */
static ALWAYS_INLINE void spinlock_lock(spinlock_t *l) {
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        while (l->locked) {
            cpu_pause();
        }
    }
}

/**
 * @brief Release the spinlock.
 */
static ALWAYS_INLINE void spinlock_unlock(spinlock_t *l) {
    __sync_lock_release(&l->locked);
}

#endif /* HELIOS_SPINLOCK_H */
