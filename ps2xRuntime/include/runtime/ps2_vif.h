#ifndef PS2_VIF_H
#define PS2_VIF_H

#include <cstdint>

// A VIF (VIF0 in front of VU0, VIF1 in front of VU1) as seen by PS2Memory.
// A host that implements one installs it with PS2Memory::setVif0Device() or
// setVif1Device(), and then that VIF's registers (VIF0 0x10003800-0x100039FF,
// VIF1 0x10003C00-0x10003DFF), its FIFO (0x10004000 / 0x10005000) and its
// DMA channel (0 / 1) go to it. VIF1 sends its GIF data (DIRECT, XGKICK) to
// PS2Memory::submitGifPacket itself.
//
// Every call is made on the EE executor thread, from inside guest memory
// accesses, so the device must not block.
class PS2VifDevice
{
public:
    virtual ~PS2VifDevice() = default;

    // offset from the VIF's first register: 0x00 STAT, 0x10 FBRST, 0x20 ERR, 0x30 MARK ...
    virtual void writeRegister(uint32_t offset, uint32_t value) = 0;
    virtual uint32_t readRegister(uint32_t offset) = 0;

    // Data words from the VIF's DMA channel or its FIFO. The device keeps what
    // it cannot process yet; the caller sends no more while it is stalled.
    virtual void pushData(const uint32_t *words, uint32_t count) = 0;

    // True while the VIF waits for FBRST.STC (an I bit, ForceBreak, STOP or an
    // error): its DMA channel pauses until it clears.
    virtual bool stalled() const = 0;

    // True once per I bit or error: raise the VIF's interrupt (INTC 4 / 5).
    virtual bool takeInterrupt() = 0;
};

#endif
