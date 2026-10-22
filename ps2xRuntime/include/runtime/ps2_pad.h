#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

class PSPadBackend
{
public:
    // Fills a 32-byte pad report the way readState() does. Called on the EE
    // thread.
    using HostReader = std::function<bool(int port, int slot, uint8_t *data, size_t size)>;

    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);

    // Lets a host with its own window and input read the pad instead of raylib.
    void setHostReader(HostReader reader) { m_hostReader = std::move(reader); }

private:
    HostReader m_hostReader;
};

#endif
