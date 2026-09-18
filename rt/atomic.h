/* The atomic operations of the language. Every one is sequentially
   consistent, and the width of the value comes with its address. */
#ifndef ANTI_ATOMIC_H
#define ANTI_ATOMIC_H

#include <stdint.h>

int64_t anti_rt_atomic_load(const void *address, int64_t width);
void anti_rt_atomic_store(void *address, int64_t width, int64_t value);
int64_t anti_rt_atomic_swap(void *address, int64_t width, int64_t value);
int64_t anti_rt_atomic_add(void *address, int64_t width, int64_t value);
int64_t anti_rt_atomic_sub(void *address, int64_t width, int64_t value);
int64_t anti_rt_atomic_and(void *address, int64_t width, int64_t value);
int64_t anti_rt_atomic_or(void *address, int64_t width, int64_t value);

/* True when the value at address was expected and is now desired. */
int8_t anti_rt_atomic_compare_swap(void *address, int64_t width,
                                   int64_t expected, int64_t desired);

#endif
