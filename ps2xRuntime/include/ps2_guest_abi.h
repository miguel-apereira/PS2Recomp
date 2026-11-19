#ifndef PS2_GUEST_ABI_H
#define PS2_GUEST_ABI_H

// Everything recompiled game code calls in the runtime, as free functions.
//
// Generated code reaches the runtime only through this header (via
// ps2_guest_macros.h) and the handlers it declares itself, never through
// ps2_runtime.h. PS2Runtime grows a member or a field whenever a host takes
// over a piece of hardware; if every generated file included its class, each
// such change would rebuild all of them. Keep this header small and stable.

#include <atomic>
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

    // Every guest transfer the dispatcher does (see PS2Runtime::dispatchGuestBranch).
    bool dispatchGuestBranchSlow(PS2Runtime *runtime,
                                 uint8_t *rdram,
                                 R5900Context *ctx,
                                 uint32_t targetPc,
                                 uint32_t sourcePc,
                                 uint32_t fallthroughPc,
                                 Ps2GuestBranchKind kind,
                                 const char *debugName);

    using GuestFunction = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
    // What the inline path of a direct call needs from the runtime, as plain
    // pointers so this header stays independent of the runtime's classes. The
    // runtime fills it in (PS2Runtime's constructor, EeScheduler::setOverclock).
    struct FastDispatch
    {
        GuestFunction *table = nullptr;
        uint32_t base = 0;
        uint32_t slots = 0;
        uint64_t *eeCycle = nullptr;                      // EeScheduler::m_eeCycle
        const std::atomic<uint64_t> *fastUntil = nullptr; // EeScheduler::m_fastUntil (see checkpointDue)
        const std::atomic<bool> *stopRequested = nullptr;
        std::atomic<uint32_t> callCycles{8u};             // guestCycles(kGuestDispatchCycles)
        bool enabled = false;                             // off with TGR_TRACE_CALLS or TGR_NO_FAST_CALLS
    };
    extern FastDispatch g_fastDispatch;
    // Dispatcher unwinds on this thread (a checkpoint stopped a transfer).
    inline thread_local uint64_t g_guestUnwinds = 0;

    // A guest transfer. A direct call (JAL) with nothing due at its
    // checkpoint goes straight through the function table here, inline in the
    // generated code, as the dispatcher would have done it (less its debug
    // history); everything else goes to the dispatcher.
    inline bool dispatchGuestBranch(PS2Runtime *runtime,
                                    uint8_t *rdram,
                                    R5900Context *ctx,
                                    uint32_t targetPc,
                                    uint32_t sourcePc,
                                    uint32_t fallthroughPc,
                                    Ps2GuestBranchKind kind,
                                    const char *debugName)
    {
        const FastDispatch &fast = g_fastDispatch;
        if (kind == Ps2GuestBranchKind::DirectCall && fast.enabled)
        {
            const uint32_t slot = (targetPc - fast.base) >> 2;
            const uint32_t cycles = fast.callCycles.load(std::memory_order_relaxed);
            if ((targetPc & 3u) == 0u && targetPc >= fast.base && slot < fast.slots &&
                *fast.eeCycle + cycles < fast.fastUntil->load(std::memory_order_relaxed))
            {
                const GuestFunction function = fast.table[slot];
                if (function != nullptr)
                {
                    *fast.eeCycle += cycles;
                    ctx->pc = targetPc;
                    const uint64_t unwindsBefore = g_guestUnwinds;
                    function(rdram, ctx, runtime);
                    if (fast.stopRequested->load(std::memory_order_relaxed) || ctx->pc == 0u)
                    {
                        return false;
                    }
                    if (ctx->pc == targetPc)
                    {
                        // As in the dispatcher: a host function that leaves the PC alone has
                        // finished; generated code back at its own entry is unwinding.
                        if (g_guestUnwinds != unwindsBefore)
                        {
                            return false;
                        }
                        ctx->pc = fallthroughPc;
                    }
                    return ctx->pc == fallthroughPc;
                }
            }
        }
        return dispatchGuestBranchSlow(runtime, rdram, ctx, targetPc, sourcePc, fallthroughPc, kind, debugName);
    }
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
