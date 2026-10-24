#ifndef PS2_GS_HOST_H
#define PS2_GS_HOST_H

#include <cstdint>

// A host's view of everything that reaches the GS. A host installs one with
// GS::setHost(), and then every GIF packet (after PATH arbitration), every
// register write and image upload the runtime makes on its own, and every
// vertical blank go to it first.
//
// Each call that returns bool lets the host take the work over: true means
// the host has handled it and the runtime's GS skips it; false means the
// runtime's GS processes it as usual (so a host that only records or counts
// returns false).
//
// Calls come from the threads that feed the GS (the EE executor, and the EE
// scheduler for vsync), so the host must not block for long.
class PS2GsHost
{
public:
    virtual ~PS2GsHost() = default;

    // A GIF packet: GIFtags with their data, as the GS would read them.
    virtual bool gifPacket(const uint8_t *data, uint32_t sizeBytes) = 0;

    // A register write that did not come through the GIF (the runtime's
    // versions of sceGsSwapDBuff and friends write registers directly).
    virtual bool registerWrite(uint8_t reg, uint64_t value) = 0;

    // A host-to-local transfer the runtime started directly: BITBLTBUF,
    // TRXPOS, TRXREG and TRXDIR are written, then the image data follows.
    virtual bool imageUpload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint64_t trxdir,
                             const uint8_t *data, uint32_t sizeBytes) = 0;

    // The runtime's shortcut that fills a context's whole frame buffer with
    // one colour (it has no GS equivalent).
    virtual bool clearFrame(uint32_t context, uint32_t rgba) = 0;

    // A local-to-host transfer is read back into dst (up to maxBytes). When
    // the host handles it, it fills dst and sets bytesRead.
    virtual bool localToHost(uint8_t *dst, uint32_t maxBytes, uint32_t &bytesRead) = 0;

    // The start of a vertical blank; tick counts them from 1.
    virtual void vsync(uint64_t tick) = 0;
};

#endif
