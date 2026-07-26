// SPDX-License-Identifier: AGPL-3.0-only

#define _GNU_SOURCE 1

#include <pthread.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define THREAD_COUNT 8
#define ITERATIONS 4000

static alignas(64) uint8_t storage[128];
static volatile uint32_t* counter = (volatile uint32_t*)(storage + 63);

static inline void force_faulting_locked_increment(volatile uint32_t* address)
{
    __asm__ __volatile__(
        "pushfq\n\t"
        "orq $0x40000, (%%rsp)\n\t"
        "popfq\n\t"
        "lock addl $1, (%0)\n\t"
        :
        : "r"(address)
        : "cc", "memory");
}

static void* worker(void* unused)
{
    unsigned i;
    (void)unused;
    for (i = 0; i < ITERATIONS; ++i)
        force_faulting_locked_increment(counter);
    return NULL;
}

int main(void)
{
    pthread_t threads[THREAD_COUNT];
    unsigned i;
    *counter = 0;

    for (i = 0; i < THREAD_COUNT; ++i) {
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0)
            return 2;
    }
    for (i = 0; i < THREAD_COUNT; ++i) {
        if (pthread_join(threads[i], NULL) != 0)
            return 3;
    }

    printf("counter=%u expected=%u\n", *counter, THREAD_COUNT * ITERATIONS);
    return *counter == THREAD_COUNT * ITERATIONS ? 0 : 1;
}
