#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

// Where a memory card port keeps its files. The sceMc* stubs resolve guest
// paths (current directory, "..", "mc0:" prefixes) and call a store with
// absolute card paths such as "/" or "/BASCUS-97133/icon.sys". Results are
// libmc result codes: 0 or a count on success, negative on failure.
//
// By default each port is a directory on the host (mc0 and mc1 next to the
// ELF, made only when something is written). A port can be given any other
// store, or none, which the game then sees as an empty slot.
namespace ps2_stubs
{
    struct MemoryCardStoreEntry
    {
        std::string name;
        bool isDirectory = false;
        uint32_t sizeBytes = 0;
        std::time_t modifiedTime = 0;
    };

    class MemoryCardStore
    {
    public:
        virtual ~MemoryCardStore() = default;

        // "/" always exists and is a directory.
        virtual bool exists(const std::string &path) = 0;
        virtual bool isDirectory(const std::string &path) = 0;
        // The directory's entries (without "." and ".."), or false if the
        // path is not a directory.
        virtual bool list(const std::string &path, std::vector<MemoryCardStoreEntry> &entries) = 0;
        virtual int32_t makeDirectory(const std::string &path) = 0;
        // A file or an empty directory.
        virtual int32_t remove(const std::string &path) = 0;
        virtual int32_t rename(const std::string &from, const std::string &to) = 0;
        // Empties the card (format and unformat).
        virtual int32_t erase() = 0;

        // PS2_FIO_O_* flags. On success handle names the open file.
        virtual int32_t open(const std::string &path, uint32_t flags, uint64_t &handle) = 0;
        virtual int32_t read(uint64_t handle, uint8_t *dst, uint32_t size) = 0;
        virtual int32_t write(uint64_t handle, const uint8_t *src, uint32_t size) = 0;
        // PS2_FIO_SEEK_* origin; the new position.
        virtual int32_t seek(uint64_t handle, int32_t offset, int32_t origin) = 0;
        virtual int32_t flush(uint64_t handle) = 0;
        virtual int32_t close(uint64_t handle) = 0;
        // Closes every open file without reporting errors (sceMcInit/End).
        virtual void closeAll() = 0;

        // Shown in the debug panel and logs.
        virtual std::string describe() const = 0;
        virtual std::string describe(const std::string &path) const = 0;
    };

    // The host-directory store for a port (the default).
    std::shared_ptr<MemoryCardStore> createDirectoryMemoryCardStore(int32_t port);

    // Puts a card in port 0 or 1; nullptr leaves the slot empty.
    void setMemoryCardStore(int32_t port, std::shared_ptr<MemoryCardStore> store);
}
