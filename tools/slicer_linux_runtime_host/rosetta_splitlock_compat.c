// SPDX-License-Identifier: AGPL-3.0-only

#define _GNU_SOURCE 1

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#if !defined(__x86_64__)
#error "Rosetta split-lock compatibility shim must be built for x86_64"
#endif

#ifndef ORCASTUDIO_SPLITLOCK_VISIBILITY
#define ORCASTUDIO_SPLITLOCK_VISIBILITY __attribute__((visibility("hidden")))
#endif

#define X86_CF (1ULL << 0)
#define X86_PF (1ULL << 2)
#define X86_AF (1ULL << 4)
#define X86_ZF (1ULL << 6)
#define X86_SF (1ULL << 7)
#define X86_OF (1ULL << 11)
#define X86_AC (1ULL << 18)
#define X86_ARITH_FLAGS (X86_CF | X86_PF | X86_AF | X86_ZF | X86_SF | X86_OF)

enum operation_kind {
    OP_INVALID = 0,
    OP_ADD,
    OP_OR,
    OP_ADC,
    OP_SBB,
    OP_AND,
    OP_SUB,
    OP_XOR,
    OP_XADD,
    OP_CMPXCHG,
    OP_XCHG,
    OP_INC,
    OP_DEC,
    OP_NOT,
    OP_NEG,
    OP_BTS,
    OP_BTR,
    OP_BTC,
    OP_CMPXCHG_PAIR
};

struct memory_reference {
    int base_register;
    int index_register;
    unsigned scale;
    int64_t displacement;
    int rip_relative;
};

struct decoded_instruction {
    enum operation_kind operation;
    struct memory_reference memory;
    unsigned width;
    unsigned register_index;
    unsigned instruction_length;
    uint64_t immediate;
    int has_immediate;
    int bit_index_from_register;
};

static struct sigaction previous_sigbus;
static volatile unsigned int emulation_gate __attribute__((aligned(64)));
static volatile sig_atomic_t emitted_logs;
static volatile sig_atomic_t compatibility_enabled;

static const int greg_index[16] = {
    REG_RAX, REG_RCX, REG_RDX, REG_RBX,
    REG_RSP, REG_RBP, REG_RSI, REG_RDI,
    REG_R8, REG_R9, REG_R10, REG_R11,
    REG_R12, REG_R13, REG_R14, REG_R15
};

static uint64_t width_mask(unsigned width)
{
    if (width >= 8)
        return UINT64_MAX;
    return (1ULL << (width * 8U)) - 1ULL;
}

static uint64_t sign_mask(unsigned width)
{
    if (width == 0U)
        return 0U;
    if (width >= 8U)
        return UINT64_C(1) << 63U;
    return UINT64_C(1) << (width * 8U - 1U);
}

static uint64_t read_register(const ucontext_t* context, unsigned index, unsigned width)
{
    const uint64_t value = (uint64_t)context->uc_mcontext.gregs[greg_index[index & 15U]];
    return value & width_mask(width);
}

static void write_register(ucontext_t* context, unsigned index, unsigned width, uint64_t value)
{
    greg_t* target = &context->uc_mcontext.gregs[greg_index[index & 15U]];
    const uint64_t masked = value & width_mask(width);
    if (width == 8) {
        *target = (greg_t)masked;
    } else if (width == 4) {
        *target = (greg_t)(uint32_t)masked;
    } else {
        const uint64_t old = (uint64_t)*target;
        *target = (greg_t)((old & ~UINT64_C(0xffff)) | masked);
    }
}

static uint64_t load_memory(const volatile uint8_t* address, unsigned width)
{
    uint64_t value = 0;
    unsigned i;
    for (i = 0; i < width; ++i)
        value |= (uint64_t)address[i] << (i * 8U);
    return value;
}

static void store_memory(volatile uint8_t* address, unsigned width, uint64_t value)
{
    unsigned i;
    for (i = 0; i < width; ++i)
        address[i] = (uint8_t)(value >> (i * 8U));
}

static int parity_even(uint8_t value)
{
    value ^= value >> 4;
    value &= 0x0fU;
    return ((0x9669U >> value) & 1U) != 0U;
}

static uint64_t flags_add(uint64_t old_flags, uint64_t left, uint64_t right,
                          uint64_t carry_in, uint64_t result, unsigned width)
{
    const uint64_t mask = width_mask(width);
    const uint64_t sign = sign_mask(width);
    const uint64_t left_value = left & mask;
    const uint64_t right_value = right & mask;
    const uint64_t truncated = result & mask;
    const __uint128_t full = (__uint128_t)left_value + right_value + carry_in;
    uint64_t flags = old_flags & ~X86_ARITH_FLAGS;
    if (full > mask)
        flags |= X86_CF;
    if (parity_even((uint8_t)truncated))
        flags |= X86_PF;
    if (((left_value ^ right_value ^ truncated) & 0x10U) != 0U)
        flags |= X86_AF;
    if (truncated == 0)
        flags |= X86_ZF;
    if ((truncated & sign) != 0)
        flags |= X86_SF;
    if (((~(left_value ^ right_value) & (left_value ^ truncated)) & sign) != 0)
        flags |= X86_OF;
    return flags;
}

static uint64_t flags_sub(uint64_t old_flags, uint64_t left, uint64_t right,
                          uint64_t borrow_in, uint64_t result, unsigned width)
{
    const uint64_t mask = width_mask(width);
    const uint64_t sign = sign_mask(width);
    const uint64_t left_value = left & mask;
    const uint64_t right_value = right & mask;
    const __uint128_t full_right = (__uint128_t)right_value + borrow_in;
    const uint64_t truncated = result & mask;
    uint64_t flags = old_flags & ~X86_ARITH_FLAGS;
    if ((__uint128_t)left_value < full_right)
        flags |= X86_CF;
    if (parity_even((uint8_t)truncated))
        flags |= X86_PF;
    if (((left_value ^ right_value ^ truncated) & 0x10U) != 0U)
        flags |= X86_AF;
    if (truncated == 0)
        flags |= X86_ZF;
    if ((truncated & sign) != 0)
        flags |= X86_SF;
    if ((((left_value ^ right_value) & (left_value ^ truncated)) & sign) != 0)
        flags |= X86_OF;
    return flags;
}

static uint64_t flags_logic(uint64_t old_flags, uint64_t result, unsigned width)
{
    const uint64_t truncated = result & width_mask(width);
    uint64_t flags = old_flags & ~X86_ARITH_FLAGS;
    if (parity_even((uint8_t)truncated))
        flags |= X86_PF;
    if (truncated == 0)
        flags |= X86_ZF;
    if ((truncated & sign_mask(width)) != 0)
        flags |= X86_SF;
    return flags;
}

static int read_signed_8(const uint8_t* bytes, unsigned* position, int64_t* result)
{
    *result = (int8_t)bytes[*position];
    *position += 1;
    return 1;
}

static int read_signed_32(const uint8_t* bytes, unsigned* position, int64_t* result)
{
    uint32_t value;
    memcpy(&value, bytes + *position, sizeof(value));
    *result = (int32_t)value;
    *position += 4;
    return 1;
}

static int parse_memory_reference(const uint8_t* bytes, unsigned* position, uint8_t modrm,
                                  uint8_t rex, struct memory_reference* memory)
{
    const unsigned mod = modrm >> 6;
    const unsigned rm_low = modrm & 7U;
    const unsigned rex_b = rex & 1U;
    const unsigned rex_x = (rex >> 1) & 1U;

    memset(memory, 0, sizeof(*memory));
    memory->base_register = -1;
    memory->index_register = -1;
    memory->scale = 1;
    if (mod == 3)
        return 0;

    if (rm_low == 4) {
        const uint8_t sib = bytes[(*position)++];
        const unsigned scale_bits = sib >> 6;
        const unsigned index_low = (sib >> 3) & 7U;
        const unsigned base_low = sib & 7U;
        memory->scale = 1U << scale_bits;
        if (!(index_low == 4U && rex_x == 0U))
            memory->index_register = (int)(index_low | (rex_x << 3));
        if (mod == 0 && base_low == 5U) {
            if (!read_signed_32(bytes, position, &memory->displacement))
                return 0;
        } else {
            memory->base_register = (int)(base_low | (rex_b << 3));
        }
    } else if (mod == 0 && rm_low == 5U) {
        memory->rip_relative = 1;
        if (!read_signed_32(bytes, position, &memory->displacement))
            return 0;
    } else {
        memory->base_register = (int)(rm_low | (rex_b << 3));
    }

    if (mod == 1) {
        int64_t displacement;
        read_signed_8(bytes, position, &displacement);
        memory->displacement += displacement;
    } else if (mod == 2) {
        int64_t displacement;
        read_signed_32(bytes, position, &displacement);
        memory->displacement += displacement;
    }
    return *position <= 15U;
}

static int decode_instruction(const uint8_t* bytes, struct decoded_instruction* decoded)
{
    unsigned position = 0;
    int lock_prefix = 0;
    int operand_16 = 0;
    int unsupported_address_prefix = 0;
    uint8_t rex = 0;
    uint8_t opcode;
    uint8_t modrm;
    unsigned group;

    memset(decoded, 0, sizeof(*decoded));
    decoded->operation = OP_INVALID;

    while (position < 15U) {
        const uint8_t prefix = bytes[position];
        if (prefix == 0xf0U) {
            lock_prefix = 1;
        } else if (prefix == 0x66U) {
            operand_16 = 1;
        } else if (prefix == 0x67U) {
            unsupported_address_prefix = 1;
        } else if (prefix == 0x2eU || prefix == 0x36U || prefix == 0x3eU || prefix == 0x26U ||
                   prefix == 0xf2U || prefix == 0xf3U) {
            /* Accepted but irrelevant for the supported integer instructions. */
        } else if (prefix == 0x64U || prefix == 0x65U) {
            return 0;
        } else {
            break;
        }
        ++position;
    }
    if (unsupported_address_prefix || position >= 15U)
        return 0;
    if ((bytes[position] & 0xf0U) == 0x40U)
        rex = bytes[position++];
    if (position >= 15U)
        return 0;

    decoded->width = (rex & 8U) ? 8U : (operand_16 ? 2U : 4U);
    opcode = bytes[position++];

    if (opcode == 0x0fU) {
        uint8_t second;
        if (position >= 15U)
            return 0;
        second = bytes[position++];
        if (position >= 15U)
            return 0;
        modrm = bytes[position++];
        group = (modrm >> 3) & 7U;
        decoded->register_index = group | (((rex >> 2) & 1U) << 3);
        if (!parse_memory_reference(bytes, &position, modrm, rex, &decoded->memory))
            return 0;

        switch (second) {
        case 0xc1U:
            decoded->operation = OP_XADD;
            break;
        case 0xb1U:
            decoded->operation = OP_CMPXCHG;
            break;
        case 0xabU:
            decoded->operation = OP_BTS;
            decoded->bit_index_from_register = 1;
            break;
        case 0xb3U:
            decoded->operation = OP_BTR;
            decoded->bit_index_from_register = 1;
            break;
        case 0xbbU:
            decoded->operation = OP_BTC;
            decoded->bit_index_from_register = 1;
            break;
        case 0xbaU:
            if (group < 5U || group > 7U || position >= 15U)
                return 0;
            decoded->operation = group == 5U ? OP_BTS : (group == 6U ? OP_BTR : OP_BTC);
            decoded->immediate = bytes[position++];
            decoded->has_immediate = 1;
            break;
        case 0xc7U:
            if (group != 1U)
                return 0;
            decoded->operation = OP_CMPXCHG_PAIR;
            decoded->width = (rex & 8U) ? 16U : 8U;
            break;
        default:
            return 0;
        }
    } else {
        if (position >= 15U)
            return 0;
        modrm = bytes[position++];
        group = (modrm >> 3) & 7U;
        decoded->register_index = group | (((rex >> 2) & 1U) << 3);
        if (!parse_memory_reference(bytes, &position, modrm, rex, &decoded->memory))
            return 0;

        switch (opcode) {
        case 0x01U: decoded->operation = OP_ADD; break;
        case 0x09U: decoded->operation = OP_OR; break;
        case 0x11U: decoded->operation = OP_ADC; break;
        case 0x19U: decoded->operation = OP_SBB; break;
        case 0x21U: decoded->operation = OP_AND; break;
        case 0x29U: decoded->operation = OP_SUB; break;
        case 0x31U: decoded->operation = OP_XOR; break;
        case 0x87U: decoded->operation = OP_XCHG; break;
        case 0x81U:
        case 0x83U: {
            int64_t immediate;
            if (group == 7U)
                return 0;
            decoded->operation = (enum operation_kind)(OP_ADD + group);
            if (opcode == 0x83U) {
                if (position >= 15U)
                    return 0;
                immediate = (int8_t)bytes[position++];
            } else if (decoded->width == 2U) {
                uint16_t raw;
                if (position + 2U > 15U)
                    return 0;
                memcpy(&raw, bytes + position, sizeof(raw));
                position += 2U;
                immediate = (int16_t)raw;
            } else {
                if (position + 4U > 15U)
                    return 0;
                read_signed_32(bytes, &position, &immediate);
            }
            decoded->immediate = (uint64_t)immediate;
            decoded->has_immediate = 1;
            break;
        }
        case 0xffU:
            if (group > 1U)
                return 0;
            decoded->operation = group == 0U ? OP_INC : OP_DEC;
            break;
        case 0xf7U:
            if (group != 2U && group != 3U)
                return 0;
            decoded->operation = group == 2U ? OP_NOT : OP_NEG;
            break;
        default:
            return 0;
        }
    }

    if (!lock_prefix && decoded->operation != OP_XCHG)
        return 0;
    if (position == 0U || position > 15U)
        return 0;
    decoded->instruction_length = position;
    return 1;
}

static uintptr_t effective_address(const ucontext_t* context, const struct decoded_instruction* decoded,
                                   uintptr_t instruction_pointer)
{
    uintptr_t address = 0;
    if (decoded->memory.rip_relative) {
        address = instruction_pointer + decoded->instruction_length;
    } else if (decoded->memory.base_register >= 0) {
        address = (uintptr_t)read_register(context, (unsigned)decoded->memory.base_register, 8);
    }
    if (decoded->memory.index_register >= 0)
        address += (uintptr_t)(read_register(context, (unsigned)decoded->memory.index_register, 8) * decoded->memory.scale);
    address += (uintptr_t)decoded->memory.displacement;
    return address;
}

static void lock_emulation_gate(void)
{
    while (__atomic_exchange_n(&emulation_gate, 1U, __ATOMIC_ACQUIRE) != 0U)
        __asm__ __volatile__("pause" ::: "memory");
}

static void unlock_emulation_gate(void)
{
    __atomic_store_n(&emulation_gate, 0U, __ATOMIC_RELEASE);
}

static void update_bit_operation(enum operation_kind operation, uint64_t* value, unsigned bit, uint64_t* flags)
{
    const uint64_t bit_mask = UINT64_C(1) << bit;
    if ((*value & bit_mask) != 0)
        *flags |= X86_CF;
    else
        *flags &= ~X86_CF;
    if (operation == OP_BTS)
        *value |= bit_mask;
    else if (operation == OP_BTR)
        *value &= ~bit_mask;
    else
        *value ^= bit_mask;
}

ORCASTUDIO_SPLITLOCK_VISIBILITY
int orcastudio_emulate_splitlock(ucontext_t* context, uintptr_t* handled_address,
                                 enum operation_kind* handled_operation)
{
    const uintptr_t instruction_pointer = (uintptr_t)context->uc_mcontext.gregs[REG_RIP];
    const uint8_t* bytes = (const uint8_t*)instruction_pointer;
    struct decoded_instruction decoded;
    uintptr_t address;
    volatile uint8_t* memory;
    uint64_t flags;
    uint64_t old_value;
    uint64_t source;
    uint64_t result;
    uint64_t carry;
    unsigned bit;

    if (!decode_instruction(bytes, &decoded))
        return 0;
    address = effective_address(context, &decoded, instruction_pointer);

    if (decoded.operation == OP_BTS || decoded.operation == OP_BTR || decoded.operation == OP_BTC) {
        const unsigned word_bits = decoded.width * 8U;
        int64_t bit_index;
        int64_t word_index;
        if (decoded.bit_index_from_register) {
            const uint64_t raw_index = read_register(context, decoded.register_index, decoded.width);
            const unsigned bits = decoded.width * 8U;
            bit_index = (int64_t)((raw_index ^ (UINT64_C(1) << (bits - 1U))) -
                                  (UINT64_C(1) << (bits - 1U)));
        } else
            bit_index = (int64_t)decoded.immediate;
        word_index = bit_index / (int64_t)word_bits;
        if (bit_index < 0 && (bit_index % (int64_t)word_bits) != 0)
            --word_index;
        address += (uintptr_t)(word_index * (int64_t)decoded.width);
        bit = (unsigned)(bit_index - word_index * (int64_t)word_bits);
    } else {
        bit = 0;
    }

    memory = (volatile uint8_t*)address;
    flags = (uint64_t)context->uc_mcontext.gregs[REG_EFL];
    lock_emulation_gate();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (decoded.operation == OP_CMPXCHG_PAIR) {
        if (decoded.width == 16U) {
            const uint64_t memory_low = load_memory(memory, 8);
            const uint64_t memory_high = load_memory(memory + 8, 8);
            const uint64_t compare_low = read_register(context, 0, 8);
            const uint64_t compare_high = read_register(context, 2, 8);
            if (memory_low == compare_low && memory_high == compare_high) {
                store_memory(memory, 8, read_register(context, 3, 8));
                store_memory(memory + 8, 8, read_register(context, 1, 8));
                flags |= X86_ZF;
            } else {
                write_register(context, 0, 8, memory_low);
                write_register(context, 2, 8, memory_high);
                flags &= ~X86_ZF;
            }
        } else {
            const uint64_t memory_value = load_memory(memory, 8);
            const uint64_t compare_value = read_register(context, 0, 4) |
                (read_register(context, 2, 4) << 32U);
            if (memory_value == compare_value) {
                const uint64_t replacement = read_register(context, 3, 4) |
                    (read_register(context, 1, 4) << 32U);
                store_memory(memory, 8, replacement);
                flags |= X86_ZF;
            } else {
                write_register(context, 0, 4, memory_value);
                write_register(context, 2, 4, memory_value >> 32U);
                flags &= ~X86_ZF;
            }
        }
    } else {
        old_value = load_memory(memory, decoded.width);
        source = decoded.has_immediate ? decoded.immediate : read_register(context, decoded.register_index, decoded.width);
        result = old_value;
        switch (decoded.operation) {
        case OP_ADD:
            result = old_value + source;
            flags = flags_add(flags, old_value, source, 0U, result, decoded.width);
            break;
        case OP_OR:
            result = old_value | source;
            flags = flags_logic(flags, result, decoded.width);
            break;
        case OP_ADC:
            carry = (flags & X86_CF) != 0;
            result = old_value + source + carry;
            flags = flags_add(flags, old_value, source, carry, result, decoded.width);
            break;
        case OP_SBB:
            carry = (flags & X86_CF) != 0;
            result = old_value - source - carry;
            flags = flags_sub(flags, old_value, source, carry, result, decoded.width);
            break;
        case OP_AND:
            result = old_value & source;
            flags = flags_logic(flags, result, decoded.width);
            break;
        case OP_SUB:
            result = old_value - source;
            flags = flags_sub(flags, old_value, source, 0U, result, decoded.width);
            break;
        case OP_XOR:
            result = old_value ^ source;
            flags = flags_logic(flags, result, decoded.width);
            break;
        case OP_XADD:
            result = old_value + source;
            flags = flags_add(flags, old_value, source, 0U, result, decoded.width);
            write_register(context, decoded.register_index, decoded.width, old_value);
            break;
        case OP_CMPXCHG: {
            const uint64_t accumulator = read_register(context, 0, decoded.width);
            flags = flags_sub(flags, accumulator, old_value, 0U, accumulator - old_value, decoded.width);
            if ((accumulator & width_mask(decoded.width)) == old_value) {
                result = source;
            } else {
                result = old_value;
                write_register(context, 0, decoded.width, old_value);
            }
            break;
        }
        case OP_XCHG:
            result = source;
            write_register(context, decoded.register_index, decoded.width, old_value);
            break;
        case OP_INC: {
            const uint64_t preserved_cf = flags & X86_CF;
            result = old_value + 1U;
            flags = (flags_add(flags, old_value, 1U, 0U, result, decoded.width) & ~X86_CF) | preserved_cf;
            break;
        }
        case OP_DEC: {
            const uint64_t preserved_cf = flags & X86_CF;
            result = old_value - 1U;
            flags = (flags_sub(flags, old_value, 1U, 0U, result, decoded.width) & ~X86_CF) | preserved_cf;
            break;
        }
        case OP_NOT:
            result = (~old_value) & width_mask(decoded.width);
            break;
        case OP_NEG:
            result = (0U - old_value) & width_mask(decoded.width);
            flags = flags_sub(flags, 0U, old_value, 0U, result, decoded.width);
            if (old_value != 0)
                flags |= X86_CF;
            else
                flags &= ~X86_CF;
            break;
        case OP_BTS:
        case OP_BTR:
        case OP_BTC:
            update_bit_operation(decoded.operation, &result, bit, &flags);
            break;
        default:
            unlock_emulation_gate();
            return 0;
        }
        store_memory(memory, decoded.width, result);
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    unlock_emulation_gate();
    context->uc_mcontext.gregs[REG_EFL] = (greg_t)(flags & ~X86_AC);
    context->uc_mcontext.gregs[REG_RIP] = (greg_t)(instruction_pointer + decoded.instruction_length);
    if (handled_address)
        *handled_address = address;
    if (handled_operation)
        *handled_operation = decoded.operation;
    return 1;
}

static void write_stderr_best_effort(const char* buffer, size_t length)
{
    const int saved_errno = errno;
    const ssize_t result = write(STDERR_FILENO, buffer, length);
    (void)result;
    errno = saved_errno;
}

static size_t append_text(char* buffer, size_t position, size_t capacity, const char* text)
{
    while (*text && position < capacity)
        buffer[position++] = *text++;
    return position;
}

static size_t append_hex(char* buffer, size_t position, size_t capacity, uintptr_t value)
{
    static const char digits[] = "0123456789abcdef";
    int shift;
    position = append_text(buffer, position, capacity, "0x");
    for (shift = (int)(sizeof(value) * 8U) - 4; shift >= 0 && position < capacity; shift -= 4)
        buffer[position++] = digits[(value >> shift) & 0x0fU];
    return position;
}

static const char* operation_name(enum operation_kind operation)
{
    switch (operation) {
    case OP_ADD: return "add";
    case OP_OR: return "or";
    case OP_ADC: return "adc";
    case OP_SBB: return "sbb";
    case OP_AND: return "and";
    case OP_SUB: return "sub";
    case OP_XOR: return "xor";
    case OP_XADD: return "xadd";
    case OP_CMPXCHG: return "cmpxchg";
    case OP_XCHG: return "xchg";
    case OP_INC: return "inc";
    case OP_DEC: return "dec";
    case OP_NOT: return "not";
    case OP_NEG: return "neg";
    case OP_BTS: return "bts";
    case OP_BTR: return "btr";
    case OP_BTC: return "btc";
    case OP_CMPXCHG_PAIR: return "cmpxchg-pair";
    default: return "unknown";
    }
}

static void log_handled(uintptr_t instruction_pointer, uintptr_t address, enum operation_kind operation)
{
    char buffer[256];
    size_t position = 0;
    if (__atomic_fetch_add(&emitted_logs, 1, __ATOMIC_RELAXED) >= 16)
        return;
    position = append_text(buffer, position, sizeof(buffer), "OrcaStudio Rosetta compatibility emulated unaligned atomic operation=");
    position = append_text(buffer, position, sizeof(buffer), operation_name(operation));
    position = append_text(buffer, position, sizeof(buffer), " rip=");
    position = append_hex(buffer, position, sizeof(buffer), instruction_pointer);
    position = append_text(buffer, position, sizeof(buffer), " address=");
    position = append_hex(buffer, position, sizeof(buffer), address);
    if (position < sizeof(buffer))
        buffer[position++] = '\n';
    write_stderr_best_effort(buffer, position);
}

static void terminate_unsupported_sigbus(uintptr_t instruction_pointer)
{
    char buffer[192];
    size_t position = 0;
    position = append_text(buffer, position, sizeof(buffer), "OrcaStudio Rosetta compatibility could not decode SIGBUS instruction at rip=");
    position = append_hex(buffer, position, sizeof(buffer), instruction_pointer);
    if (position < sizeof(buffer))
        buffer[position++] = '\n';
    write_stderr_best_effort(buffer, position);
    _exit(128 + SIGBUS);
}

__attribute__((noinline, used))
static void sigbus_handler_c(int signal_number, siginfo_t* information, void* opaque_context)
{
    ucontext_t* context = (ucontext_t*)opaque_context;
    uintptr_t address = 0;
    enum operation_kind operation = OP_INVALID;
    const uintptr_t instruction_pointer = (uintptr_t)context->uc_mcontext.gregs[REG_RIP];
    (void)signal_number;
    (void)information;

    if (compatibility_enabled && orcastudio_emulate_splitlock(context, &address, &operation)) {
        log_handled(instruction_pointer, address, operation);
        return;
    }

    if ((previous_sigbus.sa_flags & SA_SIGINFO) != 0 &&
        previous_sigbus.sa_handler != SIG_DFL && previous_sigbus.sa_handler != SIG_IGN) {
        previous_sigbus.sa_sigaction(SIGBUS, information, opaque_context);
        return;
    }
    if (previous_sigbus.sa_handler == SIG_IGN)
        terminate_unsupported_sigbus(instruction_pointer);
    if (previous_sigbus.sa_handler && previous_sigbus.sa_handler != SIG_DFL) {
        previous_sigbus.sa_handler(SIGBUS);
        return;
    }
    terminate_unsupported_sigbus(instruction_pointer);
}

__attribute__((naked, used))
static void sigbus_handler_entry(int signal_number __attribute__((unused)), siginfo_t* information __attribute__((unused)), void* opaque_context __attribute__((unused)))
{
    __asm__ __volatile__(
        "pushfq\n\t"
        "andq $-262145, (%rsp)\n\t"
        "popfq\n\t"
        "jmp sigbus_handler_c\n\t");
}

__attribute__((constructor))
static void initialize_splitlock_compatibility(void)
{
    const char* enabled = getenv("SLICER_LINUX_RUNTIME_ROSETTA_SPLITLOCK_COMPAT");
    struct sigaction action;
    if (!enabled || strcmp(enabled, "1") != 0)
        return;

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = sigbus_handler_entry;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    if (sigaction(SIGBUS, &action, &previous_sigbus) != 0) {
        static const char failure[] = "OrcaStudio failed to install Rosetta split-lock SIGBUS compatibility handler\n";
        write_stderr_best_effort(failure, sizeof(failure) - 1U);
        return;
    }
    compatibility_enabled = 1;
    {
        static const char active[] = "OrcaStudio Rosetta unaligned-atomic compatibility enabled\n";
        write_stderr_best_effort(active, sizeof(active) - 1U);
    }
}
