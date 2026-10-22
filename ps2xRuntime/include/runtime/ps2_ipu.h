#ifndef PS2_IPU_H
#define PS2_IPU_H

#include <cstddef>
#include <cstdint>

// The IPU (image processing unit: MPEG-2 macroblock decoder and colour
// converter) as seen by PS2Memory. The runtime has no IPU of its own; a host
// that implements one installs it with PS2Memory::setIpuDevice(), and then
// the IPU registers (0x10002000-0x1000203F), its FIFOs (0x10007000 out,
// 0x10007010 in) and DMA channels 3 (from IPU) and 4 (to IPU) go to it.
//
// Every call is made on the EE executor thread, from inside guest memory
// accesses, so the device must not block.
class PS2IpuDevice
{
public:
    virtual ~PS2IpuDevice() = default;

    // offset: 0x00 IPU_CMD, 0x10 IPU_CTRL, 0x20 IPU_BP, 0x30 IPU_TOP; +4 for
    // the upper word of each 64-bit register.
    virtual void writeRegister(uint32_t offset, uint32_t value) = 0;
    virtual uint32_t readRegister(uint32_t offset) = 0;

    // Input FIFO: whole quadwords, from DMA channel 4 or CPU writes.
    virtual void pushInput(const uint8_t *quadwords, uint32_t count) = 0;

    // Output FIFO: quadwords ready for DMA channel 3 or CPU reads.
    virtual uint32_t outputAvailable() const = 0;
    virtual uint32_t popOutput(uint8_t *quadwords, uint32_t maxCount) = 0;

    // True once per finished command: raise the IPU interrupt (INTC 8).
    virtual bool takeInterrupt() = 0;
};

#endif
