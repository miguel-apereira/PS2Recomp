#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <cstring>

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16)
        return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::submitAndDrain(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16)
        return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_processFn)
        return;
    if (m_queue.empty())
    {
        // Alone in the queue, it would be sent first anyway.
        m_processFn(data, sizeBytes);
        drain(); // anything the GS submitted meanwhile, on this thread
        return;
    }
    submit(pathId, data, sizeBytes, path2DirectHl);
    drain();
}

void GifArbiter::drain()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_processFn)
        return;

    // Taken out of the queue first, so a packet submitted while these are
    // processed (from the GS, on this thread) goes into the next round
    // instead of changing this one.
    while (!m_queue.empty())
    {
        std::vector<GifArbiterPacket> packets;
        packets.swap(m_queue);
        std::stable_sort(packets.begin(), packets.end(),
                         [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                         {
                             // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                             if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                             {
                                 if (a.path3Image && b.path2DirectHl)
                                     return true;
                                 if (a.path2DirectHl && b.path3Image)
                                     return false;
                             }
                             return pathPriority(a.pathId) < pathPriority(b.pathId);
                         });

        for (const GifArbiterPacket &pkt : packets)
        {
            if (!pkt.data.empty())
            {
                m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
            }
        }
        // Give the storage back to the queue for the next packets.
        packets.clear();
        if (m_queue.empty())
            m_queue.swap(packets);
    }
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
