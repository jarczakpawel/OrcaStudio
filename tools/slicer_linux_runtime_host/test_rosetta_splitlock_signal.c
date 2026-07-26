// SPDX-License-Identifier: AGPL-3.0-only

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    void* allocation = NULL;
    uint32_t input = 7;
    uint32_t exchange = 23;
    uint32_t* target;
    uint32_t* exchange_target;
    if (posix_memalign(&allocation, 64, 128) != 0)
        return 2;
    memset(allocation, 0, 128);
    target = (uint32_t*)((unsigned char*)allocation + 63);
    exchange_target = (uint32_t*)((unsigned char*)allocation + 79);
    memcpy(target, &(uint32_t){11}, sizeof(uint32_t));
    memcpy(exchange_target, &(uint32_t){31}, sizeof(uint32_t));
    __asm__ __volatile__(
        "pushfq\n\t"
        "orq $0x40000, (%%rsp)\n\t"
        "popfq\n\t"
        "lock xaddl %0, (%1)\n\t"
        "pushfq\n\t"
        "andq $-262145, (%%rsp)\n\t"
        "popfq\n\t"
        : "+a"(input)
        : "r"(target)
        : "memory", "cc");
    __asm__ __volatile__(
        "pushfq\n\t"
        "orq $0x40000, (%%rsp)\n\t"
        "popfq\n\t"
        "xchgl %0, (%1)\n\t"
        "pushfq\n\t"
        "andq $-262145, (%%rsp)\n\t"
        "popfq\n\t"
        : "+a"(exchange)
        : "r"(exchange_target)
        : "memory", "cc");
    uint32_t result;
    uint32_t exchange_result;
    memcpy(&result, target, sizeof(result));
    memcpy(&exchange_result, exchange_target, sizeof(exchange_result));
    printf("xadd-old=%u xadd-new=%u xchg-old=%u xchg-new=%u\n",
           input, result, exchange, exchange_result);
    free(allocation);
    return input == 11 && result == 18 && exchange == 31 && exchange_result == 23 ? 0 : 3;
}
