#ifndef PS2_GUEST_ABI_H
#define PS2_GUEST_ABI_H

// Everything recompiled game code calls in the runtime, as free functions.
//
// Generated code reaches the runtime only through this header (via
// ps2_guest_macros.h) and the handlers it declares itself, never through
// ps2_runtime.h. PS2Runtime grows a member or a field whenever a host takes
// over a piece of hardware; if every generated file included its class, each
// such change would rebuild all of them. Keep this header small and stable.

#include <cstdint>

#include "runtime/ps2_address.h"
#include "runtime/ps2_cpu_context.h"

class PS2Runtime;

enum class Ps2GuestBranchKind
{
    DirectJump,
    DirectCall,
    IndirectJump,
    IndirectCall,
    Return,
};

namespace ps2_guest
{
    uint8_t Load8(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint16_t Load16(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint32_t Load32(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint64_t Load64(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    __m128i Load128(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);

    void Store8(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value);
    void Store16(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value);
    void Store32(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value);
    void Store64(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value);
    void Store128(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value);

    bool dispatchGuestBranch(PS2Runtime *runtime,
                             uint8_t *rdram,
                             R5900Context *ctx,
                             uint32_t targetPc,
                             uint32_t sourcePc,
                             uint32_t fallthroughPc,
                             Ps2GuestBranchKind kind,
                             const char *debugName);
    bool eeCheckpointDue(PS2Runtime *runtime, uint32_t cycles = 32u) noexcept;
    // COP0 Count: the EE cycle counter, plus the offset MTC0 Count left in ctx->cop0_count.
    uint32_t readCop0Count(PS2Runtime *runtime, R5900Context *ctx) noexcept;
    void writeCop0Count(PS2Runtime *runtime, R5900Context *ctx, uint32_t value) noexcept;

    void SignalException(PS2Runtime *runtime, R5900Context *ctx, PS2Exception exception);
    void handleSyscall(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId);
    void handleBreak(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx);
    void handleTrap(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx);
    void handleTLBR(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx);
    void handleTLBWI(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx);
    void handleTLBWR(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx);
    void handleTLBP(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx);
    void clearLLBit(PS2Runtime *runtime, R5900Context *ctx);

    void kickGifDmaChainFromMMIO(PS2Runtime *runtime,
                                 uint8_t *rdram,
                                 R5900Context *ctx,
                                 uint32_t dPcrValue,
                                 uint32_t dStatValue,
                                 uint32_t tadr,
                                 uint32_t chcr);
    void executeVU0Microprogram(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address);
    void vu0StartMicroProgram(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address);
}

namespace ps2_stubs
{
    // Called by the wrappers of functions that have no handler yet.
    void TODO_NAMED(const char *name, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}

#endif // PS2_GUEST_ABI_H
