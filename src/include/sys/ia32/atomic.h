/*
 * $HEADER$
 */

#ifndef LAM_SYS_ATOMIC_H_INCLUDED
#define LAM_SYS_ATOMIC_H_INCLUDED

/*
 * On ia32, we use cmpxchg.
 */


#ifdef HAVE_SMP
#define LOCK "lock; "
#define MB() __asm__ __volatile__("": : :"memory")
#else
#define LOCK
#define MB()
#endif


static inline lam_atomic_mb(void)
{
    MB();
}


static inline lam_atomic_rmb(void)
{
    MB();
}


static inline lam_atomic_wmb(void)
{
    MB();
}


static inline int lam_atomic_cmpset_32(volatile uint32_t *addr,
                                       uint32_t old,
                                       uint32_t new)
{
    uint32_t ret = old;

    __asm__ __volatile (
LOCK "cmpxchgl %1,%2   \n\
      setz     %%al    \n\
      movzbl   %%al,%0 \n"
    : "+a" (ret)
    : "r" (new), "m" (*addr)
    : "memory");

    return (ret == old);
}


static inline int lam_atomic_cmpset_acq_32(volatile uint32_t *addr,
                                           uint32_t old,
                                           uint32_t new)
{
    return lam_atomic_cmpset_32(addr, old, new);
}


static inline int lam_atomic_cmpset_rel_32(volatile uint32_t *addr,
                                           uint32_t old,
                                           uint32_t new)
{
    return lam_atomic_cmpset_32(addr, old, new);
}


static inline int lam_atomic_cmpset_64(volatile uint64_t *addr,
                                       uint64_t old,
                                       uint64_t new)
{
    /* 
     * Compare EDX:EAX with m64. If equal, set ZF and load ECX:EBX into
     * m64. Else, clear ZF and load m64 into EDX:EAX.
     */

    uint64_t ret = old;
    struct { uint32_t lo; uint32_t hi; } *p = (struct lwords *) &new;

    __asm__ __volatile(
LOCK "cmpxchg8b %1\n"
    : "+A" (ret) 
    : "m" (*addr), "b" (p->lo), "c" (p->hi)
    : "memory");

    return (ret == old);
}


static inline int lam_atomic_cmpset_acq_64(volatile uint64_t *addr,
                                           uint64_t old,
                                           uint64_t new)
{
    return lam_atomic_cpmset_64(addr, old, new);
}


static inline int lam_atomic_cmpset_rel_64(volatile uint64_t *addr,
                                           uint64_t old,
                                           uint64_t new)
{
    return lam_atomic_cpmset_64(addr, old, new);
}

#endif /* ! LAM_SYS_ATOMIC_H_INCLUDED */
