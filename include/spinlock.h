#pragma once

#include <types.h>

#define SPINLOCK_INIT { 0 }

typedef struct {
    volatile u32 locked;
} spinlock_t;

static inline void cpu_relax(void)
{
    __asm__ volatile ("pause" ::: "memory");
}

static inline u64 irq_save(void)
{
    u64 flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(u64 flags)
{
    if ((flags & (1u << 9)) != 0)
        __asm__ volatile ("sti" ::: "memory");
}

static inline void spin_lock(spinlock_t *lock)
{
    u32 expected;

    for (;;) {
        expected = 0;
        if (__atomic_compare_exchange_n(&lock->locked, &expected, 1, 0,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            break;
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0)
            cpu_relax();
    }
}

static inline void spin_unlock(spinlock_t *lock)
{
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

static inline void spin_lock_irqsave(spinlock_t *lock, u64 *flags)
{
    *flags = irq_save();
    spin_lock(lock);
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, u64 flags)
{
    spin_unlock(lock);
    irq_restore(flags);
}

static inline int spin_trylock(spinlock_t *lock)
{
    u32 expected = 0;
    return __atomic_compare_exchange_n(&lock->locked, &expected, 1, 0,
                                       __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}
