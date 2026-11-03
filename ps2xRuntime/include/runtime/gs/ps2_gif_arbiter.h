#ifndef PS2_GIF_ARBITER_H
#define PS2_GIF_ARBITER_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

enum class GifPathId : uint8_t
{
    Path1 = 1,
    Path2 = 2,
    Path3 = 3,
};

struct GifArbiterPacket
{
    GifPathId pathId;
    bool path2DirectHl = false;
    bool path3Image = false;
    std::vector<uint8_t> data;
};

// Packets from the three GIF paths, sent to the GS in path priority order.
// Thread-safe: with VIF1 on a thread of its own, PATH1 and PATH2 packets
// come from that thread while the EE submits PATH3 and drains after its DMA
// transfers. A drain sends its packets to the GS while holding the lock, so
// the GS gets one path's packets at a time.
class GifArbiter
{
public:
    using ProcessPacketFn = std::function<void(const uint8_t *, uint32_t)>;

    GifArbiter() = default;
    explicit GifArbiter(ProcessPacketFn processFn);

    void setProcessPacketFn(ProcessPacketFn fn)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_processFn = std::move(fn);
    }

    void submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl = false);

    void drain();
    // submit then drain. With nothing else queued the packet goes straight
    // to the GS, without a copy.
    void submitAndDrain(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl = false);
    bool empty() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_queue.empty();
    }

private:
    // Recursive: a packet the GS processes may lead back here on the same thread.
    mutable std::recursive_mutex m_mutex;
    ProcessPacketFn m_processFn;
    std::vector<GifArbiterPacket> m_queue;

    static bool isImagePacket(const uint8_t *data, uint32_t sizeBytes);
    static uint8_t pathPriority(GifPathId id);
};

#endif
