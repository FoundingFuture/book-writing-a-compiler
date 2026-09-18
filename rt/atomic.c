#include "atomic.h"

/* DESIGN: every operation is sequentially consistent, which is the one
   memory order the language offers. The width comes with the address,
   because one symbol per operation keeps the compiler free of a table of
   names per width. A release build holds the instructions the target
   gives: `lock xadd` and `lock cmpxchg` on x86_64, and `ldaddal` and
   `casal` on an ARM64 with LSE, or a load-store-exclusive loop without
   it. */

#if defined(_MSC_VER)
#include <intrin.h>

int64_t anti_rt_atomic_load(const void *address, int64_t width)
{
    switch (width) {
    case 1: return *(const volatile char *)address;
    case 2: return *(const volatile short *)address;
    case 4: return *(const volatile int *)address;
    default: return *(const volatile long long *)address;
    }
}

void anti_rt_atomic_store(void *address, int64_t width, int64_t value)
{
    switch (width) {
    case 1: _InterlockedExchange8((char *)address, (char)value); break;
    case 2: _InterlockedExchange16((short *)address, (short)value); break;
    case 4: _InterlockedExchange((long *)address, (long)value); break;
    default: _InterlockedExchange64((long long *)address, value); break;
    }
}

int64_t anti_rt_atomic_swap(void *address, int64_t width, int64_t value)
{
    switch (width) {
    case 1: return _InterlockedExchange8((char *)address, (char)value);
    case 2: return _InterlockedExchange16((short *)address, (short)value);
    case 4: return _InterlockedExchange((long *)address, (long)value);
    default: return _InterlockedExchange64((long long *)address, value);
    }
}

int64_t anti_rt_atomic_add(void *address, int64_t width, int64_t value)
{
    switch (width) {
    case 1: return _InterlockedExchangeAdd8((char *)address, (char)value);
    case 2: return _InterlockedExchangeAdd16((short *)address, (short)value);
    case 4: return _InterlockedExchangeAdd((long *)address, (long)value);
    default: return _InterlockedExchangeAdd64((long long *)address, value);
    }
}

int64_t anti_rt_atomic_and(void *address, int64_t width, int64_t value)
{
    switch (width) {
    case 1: return _InterlockedAnd8((char *)address, (char)value);
    case 2: return _InterlockedAnd16((short *)address, (short)value);
    case 4: return _InterlockedAnd((long *)address, (long)value);
    default: return _InterlockedAnd64((long long *)address, value);
    }
}

int64_t anti_rt_atomic_or(void *address, int64_t width, int64_t value)
{
    switch (width) {
    case 1: return _InterlockedOr8((char *)address, (char)value);
    case 2: return _InterlockedOr16((short *)address, (short)value);
    case 4: return _InterlockedOr((long *)address, (long)value);
    default: return _InterlockedOr64((long long *)address, value);
    }
}

int8_t anti_rt_atomic_compare_swap(void *address, int64_t width,
                                   int64_t expected, int64_t desired)
{
    switch (width) {
    case 1:
        return _InterlockedCompareExchange8((char *)address, (char)desired,
                                            (char)expected) ==
               (char)expected;
    case 2:
        return _InterlockedCompareExchange16((short *)address, (short)desired,
                                             (short)expected) ==
               (short)expected;
    case 4:
        return _InterlockedCompareExchange((long *)address, (long)desired,
                                           (long)expected) == (long)expected;
    default:
        return _InterlockedCompareExchange64((long long *)address, desired,
                                             expected) == expected;
    }
}

#else

/* One body per width, written once through a macro, because the builtins
   take a typed pointer and the widths differ in nothing else. */
#define WIDTHS(op)                                                            \
    switch (width) {                                                          \
    case 1: op(int8_t);                                                       \
    case 2: op(int16_t);                                                      \
    case 4: op(int32_t);                                                      \
    default: op(int64_t);                                                     \
    }

int64_t anti_rt_atomic_load(const void *address, int64_t width)
{
#define LOAD(T) return __atomic_load_n((const T *)address, __ATOMIC_SEQ_CST)
    WIDTHS(LOAD);
#undef LOAD
}

void anti_rt_atomic_store(void *address, int64_t width, int64_t value)
{
#define STORE(T)                                                              \
    __atomic_store_n((T *)address, (T)value, __ATOMIC_SEQ_CST);               \
    return
    WIDTHS(STORE);
#undef STORE
}

int64_t anti_rt_atomic_swap(void *address, int64_t width, int64_t value)
{
#define SWAP(T)                                                               \
    return __atomic_exchange_n((T *)address, (T)value, __ATOMIC_SEQ_CST)
    WIDTHS(SWAP);
#undef SWAP
}

int64_t anti_rt_atomic_add(void *address, int64_t width, int64_t value)
{
#define ADD(T)                                                                \
    return __atomic_fetch_add((T *)address, (T)value, __ATOMIC_SEQ_CST)
    WIDTHS(ADD);
#undef ADD
}

int64_t anti_rt_atomic_and(void *address, int64_t width, int64_t value)
{
#define AND(T)                                                                \
    return __atomic_fetch_and((T *)address, (T)value, __ATOMIC_SEQ_CST)
    WIDTHS(AND);
#undef AND
}

int64_t anti_rt_atomic_or(void *address, int64_t width, int64_t value)
{
#define OR(T)                                                                 \
    return __atomic_fetch_or((T *)address, (T)value, __ATOMIC_SEQ_CST)
    WIDTHS(OR);
#undef OR
}

int8_t anti_rt_atomic_compare_swap(void *address, int64_t width,
                                   int64_t expected, int64_t desired)
{
#define CAS(T) {                                                              \
        T want = (T)expected;                                                 \
        return __atomic_compare_exchange_n((T *)address, &want, (T)desired,    \
                                           0, __ATOMIC_SEQ_CST,               \
                                           __ATOMIC_SEQ_CST);                 \
    }
    WIDTHS(CAS);
#undef CAS
}

#endif

int64_t anti_rt_atomic_sub(void *address, int64_t width, int64_t value)
{
    return anti_rt_atomic_add(address, width, -value);
}
