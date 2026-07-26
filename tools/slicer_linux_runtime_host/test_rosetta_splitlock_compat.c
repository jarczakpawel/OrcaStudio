// SPDX-License-Identifier: AGPL-3.0-only

#define ORCASTUDIO_SPLITLOCK_VISIBILITY
#include "rosetta_splitlock_compat.c"

#include <assert.h>
#include <stdio.h>

static void initialize_context(ucontext_t* context, uint8_t* code)
{
    memset(context, 0, sizeof(*context));
    context->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)code;
    context->uc_mcontext.gregs[REG_EFL] = 0x202;
}

static uint32_t load_u32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t load_u64(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void store_u32(uint8_t* p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}

static void store_u64(uint8_t* p, uint64_t v)
{
    memcpy(p, &v, sizeof(v));
}

static void test_xadd32(void)
{
    uint8_t code[] = {0xf0, 0x0f, 0xc1, 0x03};
    uint8_t memory[32] = {0};
    ucontext_t context;
    uintptr_t address = 0;
    enum operation_kind operation = OP_INVALID;
    store_u32(memory + 7, 10);
    initialize_context(&context, code);
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)(memory + 7);
    context.uc_mcontext.gregs[REG_RAX] = 3;
    assert(orcastudio_emulate_splitlock(&context, &address, &operation) == 1);
    assert(operation == OP_XADD);
    assert(address == (uintptr_t)(memory + 7));
    assert(load_u32(memory + 7) == 13);
    assert((uint32_t)context.uc_mcontext.gregs[REG_RAX] == 10);
    assert((uintptr_t)context.uc_mcontext.gregs[REG_RIP] == (uintptr_t)code + sizeof(code));
}


static void test_unprefixed_xchg32(void)
{
    uint8_t code[] = {0x87, 0x03};
    uint8_t memory[32] = {0};
    ucontext_t context;
    uintptr_t address = 0;
    enum operation_kind operation = OP_INVALID;
    store_u32(memory + 7, 31);
    initialize_context(&context, code);
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)(memory + 7);
    context.uc_mcontext.gregs[REG_RAX] = 23;
    assert(orcastudio_emulate_splitlock(&context, &address, &operation) == 1);
    assert(operation == OP_XCHG);
    assert(address == (uintptr_t)(memory + 7));
    assert(load_u32(memory + 7) == 23);
    assert((uint32_t)context.uc_mcontext.gregs[REG_RAX] == 31);
    assert((uintptr_t)context.uc_mcontext.gregs[REG_RIP] == (uintptr_t)code + sizeof(code));
}

static void test_sib_xadd64(void)
{
    uint8_t code[] = {0xf0, 0x48, 0x0f, 0xc1, 0x44, 0x8b, 0x08};
    uint8_t memory[96] = {0};
    ucontext_t context;
    store_u64(memory + 28, 0xffffffffffffffffULL);
    initialize_context(&context, code);
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)memory;
    context.uc_mcontext.gregs[REG_RCX] = 5;
    context.uc_mcontext.gregs[REG_RAX] = 1;
    assert(orcastudio_emulate_splitlock(&context, NULL, NULL) == 1);
    assert(load_u64(memory + 28) == 0);
    assert((uint64_t)context.uc_mcontext.gregs[REG_RAX] == UINT64_MAX);
    assert(((uint64_t)context.uc_mcontext.gregs[REG_EFL] & (X86_CF | X86_ZF)) == (X86_CF | X86_ZF));
}

static void test_cmpxchg32(void)
{
    uint8_t code[] = {0xf0, 0x0f, 0xb1, 0x0b};
    uint8_t memory[16] = {0};
    ucontext_t context;
    store_u32(memory + 3, 42);
    initialize_context(&context, code);
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)(memory + 3);
    context.uc_mcontext.gregs[REG_RAX] = 42;
    context.uc_mcontext.gregs[REG_RCX] = 99;
    assert(orcastudio_emulate_splitlock(&context, NULL, NULL) == 1);
    assert(load_u32(memory + 3) == 99);
    assert(((uint64_t)context.uc_mcontext.gregs[REG_EFL] & X86_ZF) != 0);

    initialize_context(&context, code);
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)(memory + 3);
    context.uc_mcontext.gregs[REG_RAX] = 5;
    context.uc_mcontext.gregs[REG_RCX] = 77;
    assert(orcastudio_emulate_splitlock(&context, NULL, NULL) == 1);
    assert(load_u32(memory + 3) == 99);
    assert((uint32_t)context.uc_mcontext.gregs[REG_RAX] == 99);
    assert(((uint64_t)context.uc_mcontext.gregs[REG_EFL] & X86_ZF) == 0);
}

static void test_immediate_and_unary(void)
{
    uint8_t add_code[] = {0xf0, 0x83, 0x43, 0x02, 0x01};
    uint8_t neg_code[] = {0xf0, 0xf7, 0x1b};
    uint8_t memory[32] = {0};
    ucontext_t context;
    store_u32(memory + 6, 4);
    initialize_context(&context, add_code);
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)(memory + 4);
    assert(orcastudio_emulate_splitlock(&context, NULL, NULL) == 1);
    assert(load_u32(memory + 6) == 5);

    initialize_context(&context, neg_code);
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)(memory + 6);
    assert(orcastudio_emulate_splitlock(&context, NULL, NULL) == 1);
    assert(load_u32(memory + 6) == (uint32_t)-5);
    assert(((uint64_t)context.uc_mcontext.gregs[REG_EFL] & X86_CF) != 0);
}

static void test_bit_operation(void)
{
    uint8_t code[] = {0xf0, 0x0f, 0xab, 0x0b};
    uint8_t memory[32] = {0};
    ucontext_t context;
    initialize_context(&context, code);
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)(memory + 5);
    context.uc_mcontext.gregs[REG_RCX] = 35;
    assert(orcastudio_emulate_splitlock(&context, NULL, NULL) == 1);
    assert(load_u32(memory + 9) == 8);
    assert(((uint64_t)context.uc_mcontext.gregs[REG_EFL] & X86_CF) == 0);
}

static void test_cmpxchg8b(void)
{
    uint8_t code[] = {0xf0, 0x0f, 0xc7, 0x0b};
    uint8_t memory[32] = {0};
    ucontext_t context;
    const uint64_t initial = UINT64_C(0x1122334455667788);
    const uint64_t replacement = UINT64_C(0xaabbccddeeff0011);
    store_u64(memory + 5, initial);
    initialize_context(&context, code);
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)(memory + 5);
    context.uc_mcontext.gregs[REG_RAX] = (uint32_t)initial;
    context.uc_mcontext.gregs[REG_RDX] = (uint32_t)(initial >> 32);
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)(memory + 5);
    /* cmpxchg8b uses EBX as replacement, so use RDI as the memory base. */
    code[3] = 0x0f; /* mod=00, /1, r/m=RDI */
    context.uc_mcontext.gregs[REG_RDI] = (greg_t)(uintptr_t)(memory + 5);
    context.uc_mcontext.gregs[REG_RBX] = (uint32_t)replacement;
    context.uc_mcontext.gregs[REG_RCX] = (uint32_t)(replacement >> 32);
    assert(orcastudio_emulate_splitlock(&context, NULL, NULL) == 1);
    assert(load_u64(memory + 5) == replacement);
    assert(((uint64_t)context.uc_mcontext.gregs[REG_EFL] & X86_ZF) != 0);
}

static void test_cmpxchg16b(void)
{
    uint8_t code[] = {0xf0, 0x48, 0x0f, 0xc7, 0x0f};
    uint8_t memory[48] = {0};
    ucontext_t context;
    store_u64(memory + 7, UINT64_C(0x0102030405060708));
    store_u64(memory + 15, UINT64_C(0x1112131415161718));
    initialize_context(&context, code);
    context.uc_mcontext.gregs[REG_RDI] = (greg_t)(uintptr_t)(memory + 7);
    context.uc_mcontext.gregs[REG_RAX] = UINT64_C(0x0102030405060708);
    context.uc_mcontext.gregs[REG_RDX] = UINT64_C(0x1112131415161718);
    context.uc_mcontext.gregs[REG_RBX] = UINT64_C(0x2122232425262728);
    context.uc_mcontext.gregs[REG_RCX] = UINT64_C(0x3132333435363738);
    assert(orcastudio_emulate_splitlock(&context, NULL, NULL) == 1);
    assert(load_u64(memory + 7) == UINT64_C(0x2122232425262728));
    assert(load_u64(memory + 15) == UINT64_C(0x3132333435363738));
}



static uint64_t run_native_locked_adc32(uint32_t* memory, uint32_t source)
{
    uint64_t flags;
    __asm__ __volatile__(
        "stc\n\t"
        "lock adcl %%eax, (%%rbx)\n\t"
        "pushfq\n\t"
        "popq %0\n\t"
        : "=r"(flags)
        : "a"(source), "b"(memory)
        : "cc", "memory");
    return flags;
}

static uint64_t run_native_locked_sbb32(uint32_t* memory, uint32_t source)
{
    uint64_t flags;
    __asm__ __volatile__(
        "stc\n\t"
        "lock sbbl %%eax, (%%rbx)\n\t"
        "pushfq\n\t"
        "popq %0\n\t"
        : "=r"(flags)
        : "a"(source), "b"(memory)
        : "cc", "memory");
    return flags;
}

static void test_adc_sbb_against_native_x86(void)
{
    uint8_t adc_code[] = {0xf0, 0x11, 0x03};
    uint8_t sbb_code[] = {0xf0, 0x19, 0x03};
    _Alignas(64) uint8_t native_storage[128] = {0};
    _Alignas(64) uint8_t emulated_storage[128] = {0};
    uint32_t* native_memory = (uint32_t*)(native_storage + 5);
    uint32_t* emulated_memory = (uint32_t*)(emulated_storage + 5);
    ucontext_t context;
    uint64_t native_flags;
    const uint64_t relevant = X86_ARITH_FLAGS;

    store_u32((uint8_t*)native_memory, UINT32_C(0x7fffffff));
    store_u32((uint8_t*)emulated_memory, UINT32_C(0x7fffffff));
    native_flags = run_native_locked_adc32(native_memory, 0);
    initialize_context(&context, adc_code);
    context.uc_mcontext.gregs[REG_EFL] |= X86_CF;
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)emulated_memory;
    context.uc_mcontext.gregs[REG_RAX] = 0;
    assert(orcastudio_emulate_splitlock(&context, NULL, NULL) == 1);
    assert(load_u32((uint8_t*)native_memory) == load_u32((uint8_t*)emulated_memory));
    assert((native_flags & relevant) == ((uint64_t)context.uc_mcontext.gregs[REG_EFL] & relevant));

    store_u32((uint8_t*)native_memory, UINT32_C(0x80000000));
    store_u32((uint8_t*)emulated_memory, UINT32_C(0x80000000));
    native_flags = run_native_locked_sbb32(native_memory, UINT32_C(0x7fffffff));
    initialize_context(&context, sbb_code);
    context.uc_mcontext.gregs[REG_EFL] |= X86_CF;
    context.uc_mcontext.gregs[REG_RBX] = (greg_t)(uintptr_t)emulated_memory;
    context.uc_mcontext.gregs[REG_RAX] = UINT32_C(0x7fffffff);
    assert(orcastudio_emulate_splitlock(&context, NULL, NULL) == 1);
    assert(load_u32((uint8_t*)native_memory) == load_u32((uint8_t*)emulated_memory));
    assert((native_flags & relevant) == ((uint64_t)context.uc_mcontext.gregs[REG_EFL] & relevant));
}

static void test_adc_sbb_flag_edges(void)
{
    uint64_t flags;

    flags = flags_add(0x202, UINT32_C(0x7fffffff), 0, 1, UINT32_C(0x80000000), 4);
    assert((flags & X86_OF) != 0);
    assert((flags & X86_AF) != 0);
    assert((flags & X86_CF) == 0);

    flags = flags_sub(0x202, UINT32_C(0x80000000), UINT32_C(0x7fffffff), 1, 0, 4);
    assert((flags & X86_OF) != 0);
    assert((flags & X86_AF) != 0);
    assert((flags & X86_CF) == 0);
}

static void test_unsupported(void)
{
    uint8_t code[16] = {0x90};
    ucontext_t context;
    initialize_context(&context, code);
    assert(orcastudio_emulate_splitlock(&context, NULL, NULL) == 0);
    assert((uintptr_t)context.uc_mcontext.gregs[REG_RIP] == (uintptr_t)code);
}

int main(void)
{
    test_xadd32();
    test_unprefixed_xchg32();
    test_sib_xadd64();
    test_cmpxchg32();
    test_immediate_and_unary();
    test_bit_operation();
    test_cmpxchg8b();
    test_cmpxchg16b();
    test_adc_sbb_flag_edges();
    test_adc_sbb_against_native_x86();
    test_unsupported();
    puts("Rosetta split-lock compatibility tests OK");
    return 0;
}
