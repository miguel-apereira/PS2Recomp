#ifndef PS2_VIF0_H
#define PS2_VIF0_H

#include <cstdint>

// VIF0 (the VU0 interface: unpacks data into VU0 memory, uploads and starts
// VU0 microprograms) as seen by PS2Memory. A host that implements it installs
// it with PS2Memory::setVif0Device(), and then the VIF0 registers
// (0x10003800-0x100039FF), its FIFO (0x10004000) and DMA channel 0 go to it.
//
// Every call is made on the EE executor thread, from inside guest memory
// accesses, so the device must not block.
class PS2Vif0Device
{
public:
    virtual ~PS2Vif0Device() = default;

    // offset from 0x10003800: 0x00 STAT, 0x10 FBRST, 0x20 ERR, 0x30 MARK ...
    virtual void writeRegister(uint32_t offset, uint32_t value) = 0;
    virtual uint32_t readRegister(uint32_t offset) = 0;

    // Data words from DMA channel 0 or the FIFO. The device keeps what it
    // cannot process yet; the caller sends no more while it is stalled.
    virtual void pushData(const uint32_t *words, uint32_t count) = 0;

    // True while VIF0 waits for FBRST.STC (an I bit, ForceBreak, STOP or an
    // error): DMA channel 0 pauses until it clears.
    virtual bool stalled() const = 0;

    // True once per I bit or error: raise the VIF0 interrupt (INTC 4).
    virtual bool takeInterrupt() = 0;
};

#endif
