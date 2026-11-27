#include "Common.h"
#include "MemoryCard.h"

#include "ps2_memory_card_store.h"

namespace ps2_stubs
{
    namespace
    {
        constexpr int32_t kMcCmdGetInfo = 0x01;
        constexpr int32_t kMcCmdOpen = 0x02;
        constexpr int32_t kMcCmdClose = 0x03;
        constexpr int32_t kMcCmdSeek = 0x04;
        constexpr int32_t kMcCmdRead = 0x05;
        constexpr int32_t kMcCmdWrite = 0x06;
        constexpr int32_t kMcCmdFlush = 0x0A;
        constexpr int32_t kMcCmdMkdir = 0x0B;
        constexpr int32_t kMcCmdChdir = 0x0C;
        constexpr int32_t kMcCmdGetDir = 0x0D;
        constexpr int32_t kMcCmdSetFileInfo = 0x0E;
        constexpr int32_t kMcCmdDelete = 0x0F;
        constexpr int32_t kMcCmdFormat = 0x10;
        constexpr int32_t kMcCmdUnformat = 0x11;
        constexpr int32_t kMcCmdGetEntSpace = 0x12;
        constexpr int32_t kMcCmdRename = 0x13;

        constexpr int32_t kMcResultSucceed = 0;
        constexpr int32_t kMcResultChangedCard = -1;
        constexpr int32_t kMcResultNoFormat = -2;
        constexpr int32_t kMcResultNoEntry = -4;
        constexpr int32_t kMcResultDeniedPermit = -5;
        constexpr int32_t kMcResultNotEmpty = -6;
        constexpr int32_t kMcResultUpLimitHandle = -7;
        constexpr int32_t kMcResultNoCard = -10;

        constexpr int32_t kMcTypeNone = 0;
        constexpr int32_t kMcTypePs2 = 2;
        constexpr int32_t kMcFormatted = 1;
        constexpr int32_t kMcUnformatted = 0;
        constexpr int32_t kMcFreeClusters = 0x2000;
        constexpr size_t kMcMaxPathLen = 1024;
        constexpr size_t kMcMaxOpenFiles = 32;

        constexpr uint16_t kMcAttrReadable = 0x0001;
        constexpr uint16_t kMcAttrWriteable = 0x0002;
        constexpr uint16_t kMcAttrFile = 0x0010;
        constexpr uint16_t kMcAttrSubdir = 0x0020;
        constexpr uint16_t kMcAttrClosed = 0x0080;
        constexpr uint16_t kMcAttrExists = 0x8000;

        struct SceMcStDateTime
        {
            uint8_t Resv2 = 0;
            uint8_t Sec = 0;
            uint8_t Min = 0;
            uint8_t Hour = 0;
            uint8_t Day = 0;
            uint8_t Month = 0;
            uint16_t Year = 0;
        };

        struct SceMcTblGetDir
        {
            SceMcStDateTime _Create{};
            SceMcStDateTime _Modify{};
            uint32_t FileSizeByte = 0;
            uint16_t AttrFile = 0;
            uint16_t Reserve1 = 0;
            uint32_t Reserve2 = 0;
            uint32_t PdaAplNo = 0;
            char EntryName[32]{};
        };

        static_assert(sizeof(SceMcTblGetDir) == 64, "sceMcTblGetDir size mismatch");

        struct McOpenFile
        {
            int32_t port = 0;
            uint64_t handle = 0;
            std::filesystem::path hostPath;
        };

        struct McPortState
        {
            std::string currentDir = "/";
            bool formatted = true;
            // The last sceMcGetDir listing (mode 0) and how far into it the
            // calls have read: a call with a nonzero mode continues it.
            std::vector<SceMcTblGetDir> dirListing;
            size_t dirCursor = 0;
        };

        std::mutex g_mcStateMutex;
        int32_t g_mcNextFd = 1;
        int32_t g_mcLastCmd = 0;
        bool g_mcCommandPending = false;
        int32_t g_mcLastResult = 0;
        std::unordered_map<int32_t, McOpenFile> g_mcFiles;
        std::array<McPortState, 2> g_mcPorts{};
        // The card in each port. A port nobody configured gets the host
        // directory store the first time it is used.
        std::array<std::shared_ptr<MemoryCardStore>, 2> g_mcStores{};
        std::array<bool, 2> g_mcStoreConfigured{};
        int32_t g_cvMcFileCursor = 0;

        constexpr int32_t kCvMcSaveFileBytes = 0x838;
        constexpr int32_t kCvMcConfigFileBytes = 0x34;
        constexpr int32_t kCvMcIconInfoBytes = 0x3C4;
        constexpr int32_t kCvMcIconFileBytes = 0xB3F8;

        constexpr int32_t cvMcKilobytes(int32_t bytes)
        {
            return (bytes + 1023) / 1024;
        }

        constexpr int32_t kCvMcRequiredFreeKb =
            cvMcKilobytes(kCvMcSaveFileBytes) * 15 +
            cvMcKilobytes(kCvMcConfigFileBytes) +
            cvMcKilobytes(kCvMcIconInfoBytes) +
            cvMcKilobytes(kCvMcIconFileBytes) + 11;

        std::filesystem::path getMcRootPath(int32_t port)
        {
            const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
            std::filesystem::path root = paths.mcRoot;
            if (root.empty())
            {
                if (!paths.elfDirectory.empty())
                {
                    root = paths.elfDirectory / "mc0";
                }
                else
                {
                    std::error_code ec;
                    const std::filesystem::path cwd = std::filesystem::current_path(ec);
                    root = ec ? std::filesystem::path("mc0") : (cwd / "mc0");
                }
            }

            root = root.lexically_normal();
            if (port <= 0)
            {
                return root;
            }

            const std::filesystem::path parent = root.parent_path();
            const std::string leaf = root.filename().string();
            const std::string lowerLeaf = toLowerAscii(leaf);
            if (lowerLeaf == "mc0")
            {
                return (parent / "mc1").lexically_normal();
            }
            if (leaf.empty())
            {
                return (root / "mc1").lexically_normal();
            }

            return (parent / (leaf + "_slot" + std::to_string(port))).lexically_normal();
        }

        std::time_t fileTimeToTimeTMc(std::filesystem::file_time_type value)
        {
            const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(systemTime);
        }

        FILE *openMcHostFile(const std::filesystem::path &hostPath, uint32_t flags)
        {
            const uint32_t access = flags & PS2_FIO_O_RDWR;
            const bool read = (access == PS2_FIO_O_RDONLY) || (access == PS2_FIO_O_RDWR);
            const bool write = (access == PS2_FIO_O_WRONLY) || (access == PS2_FIO_O_RDWR);
            const bool append = (flags & PS2_FIO_O_APPEND) != 0u;
            const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
            const bool truncate = (flags & PS2_FIO_O_TRUNC) != 0u;

            std::error_code ec;
            const bool exists = std::filesystem::exists(hostPath, ec) && !ec;

            const char *mode = "rb";
            if (read && write)
            {
                if (append)
                {
                    mode = exists ? "a+b" : "w+b";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "w+b";
                }
                else
                {
                    mode = "r+b";
                }
            }
            else if (write)
            {
                if (append)
                {
                    mode = exists ? "ab" : "wb";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "wb";
                }
                else
                {
                    mode = "r+b";
                }
            }

            return std::fopen(hostPath.string().c_str(), mode);
        }

        // Card files as files in a host directory. The directory is made the
        // first time something is written, so a game that only looks at the
        // card leaves nothing behind.
        class DirectoryMemoryCardStore final : public MemoryCardStore
        {
        public:
            explicit DirectoryMemoryCardStore(int32_t port) : m_port(port) {}
            ~DirectoryMemoryCardStore() override { closeAll(); }

            bool exists(const std::string &path) override
            {
                if (path == "/")
                {
                    return true;
                }
                std::error_code ec;
                return std::filesystem::exists(hostPath(path), ec) && !ec;
            }

            bool isDirectory(const std::string &path) override
            {
                if (path == "/")
                {
                    return true;
                }
                std::error_code ec;
                return std::filesystem::is_directory(hostPath(path), ec) && !ec;
            }

            bool list(const std::string &path, std::vector<MemoryCardStoreEntry> &entries) override
            {
                entries.clear();
                const std::filesystem::path dir = hostPath(path);
                std::error_code ec;
                if (!std::filesystem::is_directory(dir, ec) || ec)
                {
                    // The root counts as an empty directory until it is made.
                    return path == "/";
                }

                const std::time_t now = std::time(nullptr);
                for (const auto &entry : std::filesystem::directory_iterator(
                         dir, std::filesystem::directory_options::skip_permission_denied, ec))
                {
                    if (ec)
                    {
                        break;
                    }
                    MemoryCardStoreEntry row{};
                    row.name = entry.path().filename().string();
                    std::error_code entryEc;
                    row.isDirectory = entry.is_directory(entryEc) && !entryEc;
                    entryEc.clear();
                    row.sizeBytes = row.isDirectory ? 0u : static_cast<uint32_t>(entry.file_size(entryEc));
                    entryEc.clear();
                    const auto written = entry.last_write_time(entryEc);
                    row.modifiedTime = entryEc ? now : fileTimeToTimeTMc(written);
                    entries.push_back(std::move(row));
                }
                return true;
            }

            int32_t makeDirectory(const std::string &path) override
            {
                ensureRoot();
                const std::filesystem::path target = hostPath(path);
                std::error_code ec;
                if (std::filesystem::exists(target, ec) && !ec)
                {
                    return std::filesystem::is_directory(target, ec) ? kMcResultSucceed : kMcResultDeniedPermit;
                }
                if (std::filesystem::create_directory(target, ec) && !ec)
                {
                    return kMcResultSucceed;
                }
                return std::filesystem::exists(target.parent_path(), ec) && !ec ? kMcResultDeniedPermit
                                                                                 : kMcResultNoEntry;
            }

            int32_t remove(const std::string &path) override
            {
                if (path == "/")
                {
                    return kMcResultNoEntry;
                }
                const std::filesystem::path target = hostPath(path);
                std::error_code ec;
                if (!std::filesystem::exists(target, ec) || ec)
                {
                    return kMcResultNoEntry;
                }
                if (std::filesystem::is_directory(target, ec) && !std::filesystem::is_empty(target, ec))
                {
                    return kMcResultNotEmpty;
                }
                return std::filesystem::remove(target, ec) && !ec ? kMcResultSucceed : kMcResultDeniedPermit;
            }

            int32_t rename(const std::string &from, const std::string &to) override
            {
                const std::filesystem::path oldPath = hostPath(from);
                const std::filesystem::path newPath = hostPath(to);
                std::error_code ec;
                if (!std::filesystem::exists(oldPath, ec) || ec ||
                    !std::filesystem::exists(newPath.parent_path(), ec) || ec)
                {
                    return kMcResultNoEntry;
                }
                std::filesystem::rename(oldPath, newPath, ec);
                return ec ? kMcResultDeniedPermit : kMcResultSucceed;
            }

            int32_t erase() override
            {
                closeAll();
                std::error_code ec;
                std::filesystem::remove_all(getMcRootPath(m_port), ec);
                return ec ? kMcResultDeniedPermit : kMcResultSucceed;
            }

            int32_t open(const std::string &path, uint32_t flags, uint64_t &handle) override
            {
                const std::filesystem::path target = hostPath(path);
                std::error_code ec;
                const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
                const bool exists = std::filesystem::exists(target, ec) && !ec;
                if (path == "/" || (exists && std::filesystem::is_directory(target, ec)))
                {
                    return kMcResultDeniedPermit;
                }
                if (!exists && !create)
                {
                    return kMcResultNoEntry;
                }
                if (!exists)
                {
                    ensureRoot();
                }
                if (!std::filesystem::exists(target.parent_path(), ec) || ec)
                {
                    return kMcResultNoEntry;
                }

                FILE *file = openMcHostFile(target, flags);
                if (!file)
                {
                    return exists ? kMcResultDeniedPermit : kMcResultNoEntry;
                }
                handle = m_nextHandle++;
                m_files.emplace(handle, file);
                return kMcResultSucceed;
            }

            int32_t read(uint64_t handle, uint8_t *dst, uint32_t size) override
            {
                FILE *file = find(handle);
                if (!file)
                {
                    return kMcResultNoEntry;
                }
                const size_t bytesRead = std::fread(dst, 1u, size, file);
                if (std::ferror(file))
                {
                    std::clearerr(file);
                    return kMcResultDeniedPermit;
                }
                return static_cast<int32_t>(bytesRead);
            }

            int32_t write(uint64_t handle, const uint8_t *src, uint32_t size) override
            {
                FILE *file = find(handle);
                if (!file)
                {
                    return kMcResultNoEntry;
                }
                const size_t bytesWritten = std::fwrite(src, 1u, size, file);
                if (std::ferror(file))
                {
                    std::clearerr(file);
                    return kMcResultDeniedPermit;
                }
                std::fflush(file);
                return static_cast<int32_t>(bytesWritten);
            }

            int32_t seek(uint64_t handle, int32_t offset, int32_t origin) override
            {
                FILE *file = find(handle);
                if (!file)
                {
                    return kMcResultNoEntry;
                }
                int whence = SEEK_SET;
                if (origin == PS2_FIO_SEEK_CUR)
                {
                    whence = SEEK_CUR;
                }
                else if (origin == PS2_FIO_SEEK_END)
                {
                    whence = SEEK_END;
                }
                if (std::fseek(file, offset, whence) != 0)
                {
                    return kMcResultDeniedPermit;
                }
                const long position = std::ftell(file);
                return position >= 0 ? static_cast<int32_t>(position) : kMcResultDeniedPermit;
            }

            int32_t flush(uint64_t handle) override
            {
                FILE *file = find(handle);
                if (!file)
                {
                    return kMcResultNoEntry;
                }
                return std::fflush(file) == 0 ? kMcResultSucceed : kMcResultDeniedPermit;
            }

            int32_t close(uint64_t handle) override
            {
                auto it = m_files.find(handle);
                if (it == m_files.end())
                {
                    return kMcResultNoEntry;
                }
                const int closed = std::fclose(it->second);
                m_files.erase(it);
                return closed == 0 ? kMcResultSucceed : kMcResultNoEntry;
            }

            void closeAll() override
            {
                for (auto &[handle, file] : m_files)
                {
                    std::fclose(file);
                }
                m_files.clear();
            }

            std::string describe() const override
            {
                return getMcRootPath(m_port).string();
            }

            std::string describe(const std::string &path) const override
            {
                return hostPath(path).string();
            }

        private:
            std::filesystem::path hostPath(const std::string &path) const
            {
                std::filesystem::path resolved = getMcRootPath(m_port);
                if (path.size() > 1u)
                {
                    resolved /= std::filesystem::path(path.substr(1));
                }
                return resolved.lexically_normal();
            }

            void ensureRoot() const
            {
                std::error_code ec;
                std::filesystem::create_directories(getMcRootPath(m_port), ec);
            }

            FILE *find(uint64_t handle) const
            {
                auto it = m_files.find(handle);
                return it == m_files.end() ? nullptr : it->second;
            }

            int32_t m_port = 0;
            uint64_t m_nextHandle = 1;
            std::unordered_map<uint64_t, FILE *> m_files;
        };

        bool isValidMcPort(int32_t port)
        {
            return port >= 0 && port < static_cast<int32_t>(g_mcPorts.size());
        }

        bool isValidMcPortSlot(int32_t port, int32_t slot)
        {
            return isValidMcPort(port) && slot == 0;
        }

        // The card in a valid port, or null when the slot is empty.
        MemoryCardStore *storeForPortLocked(int32_t port)
        {
            if (!isValidMcPort(port))
            {
                return nullptr;
            }
            const size_t index = static_cast<size_t>(port);
            if (!g_mcStoreConfigured[index])
            {
                g_mcStores[index] = createDirectoryMemoryCardStore(port);
                g_mcStoreConfigured[index] = true;
            }
            return g_mcStores[index].get();
        }

        std::vector<std::string> splitMcPathComponents(const std::string &value)
        {
            std::vector<std::string> parts;
            std::string current;
            for (char c : value)
            {
                if (c == '/' || c == '\\')
                {
                    if (!current.empty())
                    {
                        parts.push_back(current);
                        current.clear();
                    }
                }
                else
                {
                    current.push_back(c);
                }
            }

            if (!current.empty())
            {
                parts.push_back(current);
            }

            return parts;
        }

        std::string joinMcPathComponents(const std::vector<std::string> &parts)
        {
            if (parts.empty())
            {
                return "/";
            }

            std::string joined = "/";
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i != 0u)
                {
                    joined.push_back('/');
                }
                joined.append(parts[i]);
            }
            return joined;
        }

        std::string normalizeGuestMcPathLocked(int32_t port, std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            const std::string lower = toLowerAscii(path);
            if (lower.rfind("mc0:", 0) == 0 || lower.rfind("mc1:", 0) == 0)
            {
                path = path.substr(4);
            }

            const bool absolute = !path.empty() && path.front() == '/';
            std::vector<std::string> parts;
            if (!absolute && isValidMcPort(port))
            {
                parts = splitMcPathComponents(g_mcPorts[static_cast<size_t>(port)].currentDir);
            }

            for (const std::string &part : splitMcPathComponents(path))
            {
                if (part.empty() || part == ".")
                {
                    continue;
                }

                if (part == "..")
                {
                    if (!parts.empty())
                    {
                        parts.pop_back();
                    }
                    continue;
                }

                parts.push_back(part);
            }

            return joinMcPathComponents(parts);
        }

        // "/a/b" -> "/a" and "b"; "/" -> "/" and "".
        void splitMcParent(const std::string &path, std::string &parent, std::string &leaf)
        {
            const size_t slash = path.find_last_of('/');
            if (slash == std::string::npos || path == "/")
            {
                parent = "/";
                leaf.clear();
                return;
            }
            parent = slash == 0u ? std::string("/") : path.substr(0, slash);
            leaf = path.substr(slash + 1u);
        }

        bool localtimeSafeMc(const std::time_t *value, std::tm *out)
        {
#ifdef _WIN32
            return localtime_s(out, value) == 0;
#else
            return localtime_r(value, out) != nullptr;
#endif
        }

        void writeMcCString(uint8_t *rdram, uint32_t addr, const std::string &value)
        {
            if (addr == 0u)
            {
                return;
            }

            uint8_t *dst = getMemPtr(rdram, addr);
            if (!dst)
            {
                return;
            }

            std::memcpy(dst, value.c_str(), value.size() + 1u);
        }

        void writeMcDateTime(SceMcStDateTime &out, std::time_t value)
        {
            std::tm tm{};
            if (!localtimeSafeMc(&value, &tm))
            {
                std::memset(&out, 0, sizeof(out));
                return;
            }

            out.Resv2 = 0;
            out.Sec = static_cast<uint8_t>(tm.tm_sec);
            out.Min = static_cast<uint8_t>(tm.tm_min);
            out.Hour = static_cast<uint8_t>(tm.tm_hour);
            out.Day = static_cast<uint8_t>(tm.tm_mday);
            out.Month = static_cast<uint8_t>(tm.tm_mon + 1);
            out.Year = static_cast<uint16_t>(tm.tm_year + 1900);
        }

        void fillMcDirTableEntry(SceMcTblGetDir &entry,
                                 const std::string &name,
                                 bool isDirectory,
                                 uint32_t sizeBytes,
                                 std::time_t modifiedTime)
        {
            std::memset(&entry, 0, sizeof(entry));
            writeMcDateTime(entry._Create, modifiedTime);
            writeMcDateTime(entry._Modify, modifiedTime);
            entry.FileSizeByte = isDirectory ? 0u : sizeBytes;
            entry.AttrFile = static_cast<uint16_t>(kMcAttrReadable |
                                                   kMcAttrWriteable |
                                                   (isDirectory ? kMcAttrSubdir : kMcAttrFile) |
                                                   kMcAttrClosed |
                                                   kMcAttrExists);
            std::strncpy(entry.EntryName, name.c_str(), sizeof(entry.EntryName) - 1u);
            entry.EntryName[sizeof(entry.EntryName) - 1u] = '\0';
        }

        // Copies up to maxEntries entries of the port's listing, from where the
        // last call stopped, to the guest table; the count, or an error.
        int32_t copyMcDirEntries(uint8_t *rdram, McPortState &state, int32_t maxEntries, uint32_t tableAddr)
        {
            const size_t remaining =
                state.dirCursor < state.dirListing.size() ? state.dirListing.size() - state.dirCursor : 0u;
            const size_t entryCount = std::min(remaining, maxEntries > 0 ? static_cast<size_t>(maxEntries) : 0u);
            if (entryCount == 0u || tableAddr == 0u)
            {
                return static_cast<int32_t>(entryCount);
            }
            uint8_t *dst = getMemPtr(rdram, tableAddr);
            if (!dst)
            {
                return kMcResultDeniedPermit;
            }
            std::memcpy(dst, state.dirListing.data() + state.dirCursor, entryCount * sizeof(SceMcTblGetDir));
            state.dirCursor += entryCount;
            return static_cast<int32_t>(entryCount);
        }

        bool wildcardMatch(const std::string &pattern, const std::string &value)
        {
            size_t patternPos = 0u;
            size_t valuePos = 0u;
            size_t starPos = std::string::npos;
            size_t matchPos = 0u;

            while (valuePos < value.size())
            {
                if (patternPos < pattern.size() &&
                    (pattern[patternPos] == '?' || pattern[patternPos] == value[valuePos]))
                {
                    ++patternPos;
                    ++valuePos;
                }
                else if (patternPos < pattern.size() && pattern[patternPos] == '*')
                {
                    starPos = patternPos++;
                    matchPos = valuePos;
                }
                else if (starPos != std::string::npos)
                {
                    patternPos = starPos + 1u;
                    valuePos = ++matchPos;
                }
                else
                {
                    return false;
                }
            }

            while (patternPos < pattern.size() && pattern[patternPos] == '*')
            {
                ++patternPos;
            }

            return patternPos == pattern.size();
        }

        void setMcCommandResultLocked(int32_t cmd, int32_t result)
        {
            g_mcLastCmd = cmd;
            g_mcLastResult = result;
            g_mcCommandPending = true;
        }

        void closeMcFilesLocked()
        {
            for (const std::shared_ptr<MemoryCardStore> &store : g_mcStores)
            {
                if (store)
                {
                    store->closeAll();
                }
            }
            g_mcFiles.clear();
        }

        void closeMcFilesForPortLocked(int32_t port)
        {
            MemoryCardStore *store = isValidMcPort(port) ? g_mcStores[static_cast<size_t>(port)].get() : nullptr;
            for (auto it = g_mcFiles.begin(); it != g_mcFiles.end();)
            {
                if (it->second.port == port)
                {
                    if (store)
                    {
                        store->close(it->second.handle);
                    }
                    it = g_mcFiles.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        int32_t allocateMcFdLocked(int32_t port, uint64_t handle, const std::filesystem::path &hostPath)
        {
            if (g_mcFiles.size() >= kMcMaxOpenFiles)
            {
                return kMcResultUpLimitHandle;
            }

            for (int attempt = 0; attempt < 0x10000; ++attempt)
            {
                if (g_mcNextFd <= 0)
                {
                    g_mcNextFd = 1;
                }

                const int32_t fd = g_mcNextFd++;
                if (g_mcFiles.find(fd) != g_mcFiles.end())
                {
                    continue;
                }

                g_mcFiles.emplace(fd, McOpenFile{port, handle, hostPath});
                return fd;
            }

            return kMcResultUpLimitHandle;
        }

        // The store and open file behind a descriptor, or nulls.
        MemoryCardStore *storeForFdLocked(int32_t fd, McOpenFile *&openFile)
        {
            auto it = g_mcFiles.find(fd);
            if (it == g_mcFiles.end())
            {
                openFile = nullptr;
                return nullptr;
            }
            openFile = &it->second;
            return g_mcStores[static_cast<size_t>(it->second.port)].get();
        }

        // Runs op on the port's card when the port, slot and card are usable;
        // otherwise the libmc error for that state.
        template <typename Op>
        int32_t withFormattedCardLocked(int32_t port, int32_t slot, Op &&op)
        {
            if (!isValidMcPortSlot(port, slot))
            {
                return kMcResultNoEntry;
            }
            MemoryCardStore *store = storeForPortLocked(port);
            if (!store)
            {
                return kMcResultNoCard;
            }
            McPortState &state = g_mcPorts[static_cast<size_t>(port)];
            if (!state.formatted)
            {
                return kMcResultNoFormat;
            }
            return op(*store, state);
        }
    }

    std::shared_ptr<MemoryCardStore> createDirectoryMemoryCardStore(int32_t port)
    {
        return std::make_shared<DirectoryMemoryCardStore>(port);
    }

    void setMemoryCardStore(int32_t port, std::shared_ptr<MemoryCardStore> store)
    {
        if (!isValidMcPort(port))
        {
            return;
        }
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        closeMcFilesForPortLocked(port);
        const size_t index = static_cast<size_t>(port);
        g_mcStores[index] = std::move(store);
        g_mcStoreConfigured[index] = true;
        g_mcPorts[index].currentDir = "/";
        g_mcPorts[index].dirListing.clear();
        g_mcPorts[index].dirCursor = 0;
    }

    MemoryCardDebugSnapshot getMemoryCardDebugSnapshot()
    {
        MemoryCardDebugSnapshot snapshot{};
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        snapshot.nextFd = g_mcNextFd;
        snapshot.lastCmd = g_mcLastCmd;
        snapshot.lastResult = g_mcLastResult;
        snapshot.cvFileCursor = g_cvMcFileCursor;

        for (size_t i = 0; i < g_mcPorts.size(); ++i)
        {
            snapshot.ports[i].port = static_cast<int32_t>(i);
            snapshot.ports[i].currentDir = g_mcPorts[i].currentDir;
            snapshot.ports[i].formatted = g_mcPorts[i].formatted;
            MemoryCardStore *store = storeForPortLocked(static_cast<int32_t>(i));
            snapshot.ports[i].rootPath = store ? std::filesystem::path(store->describe())
                                               : std::filesystem::path("(no card)");
        }

        snapshot.openFiles.reserve(g_mcFiles.size());
        for (const auto &[fd, file] : g_mcFiles)
        {
            MemoryCardDebugOpenFile row{};
            row.fd = fd;
            row.port = file.port;
            row.hostPath = file.hostPath;
            snapshot.openFiles.push_back(std::move(row));
        }
        std::sort(snapshot.openFiles.begin(), snapshot.openFiles.end(), [](const MemoryCardDebugOpenFile &a, const MemoryCardDebugOpenFile &b)
                  { return a.fd < b.fd; });
        return snapshot;
    }

    void sceMcChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceMcChdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t pathAddr = getRegU32(ctx, 6);
        const uint32_t currentDirAddr = getRegU32(ctx, 7);
        const std::string requestedDir =
            (pathAddr != 0u) ? readPs2CStringBounded(rdram, pathAddr, kMcMaxPathLen) : std::string{};

        std::string currentDir = "/";
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPort(port))
            {
                currentDir = g_mcPorts[static_cast<size_t>(port)].currentDir;
            }
            result = withFormattedCardLocked(port, slot, [&](MemoryCardStore &store, McPortState &state)
                                             {
                const std::string resolvedDir =
                    requestedDir.empty() ? state.currentDir : normalizeGuestMcPathLocked(port, requestedDir);
                if (!store.isDirectory(resolvedDir))
                {
                    return kMcResultNoEntry;
                }
                state.currentDir = resolvedDir;
                currentDir = resolvedDir;
                return kMcResultSucceed; });

            setMcCommandResultLocked(kMcCmdChdir, result);
        }

        RUNTIME_LOG("[MC] Chdir port=" << port << " '" << requestedDir
                                       << "' -> result=" << result << " cwd='" << currentDir << "'");
        writeMcCString(rdram, currentDirAddr, currentDir);
        setReturnS32(ctx, 0);
    }

    void sceMcClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            McOpenFile *openFile = nullptr;
            MemoryCardStore *store = storeForFdLocked(fd, openFile);
            if (openFile)
            {
                result = store ? store->close(openFile->handle) : kMcResultSucceed;
                g_mcFiles.erase(fd);
            }
            setMcCommandResultLocked(kMcCmdClose, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            result = withFormattedCardLocked(port, slot, [&](MemoryCardStore &store, McPortState &)
                                             { return store.remove(normalizeGuestMcPathLocked(port, path)); });
            setMcCommandResultLocked(kMcCmdDelete, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
            }
        }

        // libmc teardown is just a local state reset in this immediate runtime model.
        setReturnS32(ctx, 0);
    }

    void sceMcFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            McOpenFile *openFile = nullptr;
            if (MemoryCardStore *store = storeForFdLocked(fd, openFile))
            {
                result = store->flush(openFile->handle);
            }
            setMcCommandResultLocked(kMcCmdFlush, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcFormat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                MemoryCardStore *store = storeForPortLocked(port);
                if (!store)
                {
                    result = kMcResultNoCard;
                }
                else
                {
                    closeMcFilesForPortLocked(port);
                    result = store->erase();
                    if (result == kMcResultSucceed)
                    {
                        McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                        state.currentDir = "/";
                        state.formatted = true;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdFormat, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcGetDir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string rawPath = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        // 0 starts a listing; anything else continues the previous one where
        // the last call stopped (games read a directory a few entries at a time).
        const int32_t mode = static_cast<int32_t>(getRegU32(ctx, 7));
        const int32_t maxEntries = static_cast<int32_t>(getRegU32(ctx, 8));
        const uint32_t tableAddr = getRegU32(ctx, 9);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            result = withFormattedCardLocked(port, slot, [&](MemoryCardStore &store, McPortState &state)
                                             {
                if (mode != 0)
                {
                    return copyMcDirEntries(rdram, state, maxEntries, tableAddr);
                }

                const std::string guestQuery =
                    normalizeGuestMcPathLocked(port, rawPath.empty() ? "." : rawPath);
                const bool hasWildcard =
                    guestQuery.find('*') != std::string::npos || guestQuery.find('?') != std::string::npos;

                std::string parent;
                std::string pattern;
                if (!hasWildcard && store.isDirectory(guestQuery))
                {
                    parent = guestQuery;
                    pattern = "*";
                }
                else
                {
                    splitMcParent(guestQuery, parent, pattern);
                }
                if (pattern.empty())
                {
                    pattern = "*";
                }

                std::vector<MemoryCardStoreEntry> rows;
                state.dirListing.clear();
                state.dirCursor = 0;
                if (!store.list(parent, rows))
                {
                    return kMcResultNoEntry;
                }

                std::sort(rows.begin(), rows.end(), [](const MemoryCardStoreEntry &lhs, const MemoryCardStoreEntry &rhs)
                          { return toLowerAscii(lhs.name) < toLowerAscii(rhs.name); });

                const std::time_t now = std::time(nullptr);
                for (const char *special : {".", ".."})
                {
                    if (wildcardMatch(pattern, special))
                    {
                        SceMcTblGetDir entry{};
                        fillMcDirTableEntry(entry, special, true, 0u, now);
                        state.dirListing.push_back(entry);
                    }
                }
                for (const MemoryCardStoreEntry &row : rows)
                {
                    if (!wildcardMatch(pattern, row.name))
                    {
                        continue;
                    }
                    SceMcTblGetDir entry{};
                    fillMcDirTableEntry(entry, row.name, row.isDirectory, row.sizeBytes, row.modifiedTime);
                    state.dirListing.push_back(entry);
                }
                return copyMcDirEntries(rdram, state, maxEntries, tableAddr); });

            setMcCommandResultLocked(kMcCmdGetDir, result);
        }
        RUNTIME_LOG("[MC] GetDir port=" << port << " '" << rawPath << "' mode=" << mode
                                        << " maxent=" << maxEntries << " -> result=" << result);
        setReturnS32(ctx, 0);
    }

    void sceMcGetEntSpace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            setMcCommandResultLocked(kMcCmdGetEntSpace, 1024);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcGetInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t typePtr = getRegU32(ctx, 6);
        const uint32_t freePtr = getRegU32(ctx, 7);
        const uint32_t formatPtr = getRegU32(ctx, 8);

        int32_t cardType = kMcTypeNone;
        int32_t freeBlocks = 0;
        int32_t format = kMcUnformatted;
        int32_t result = kMcResultNoEntry;

        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                if (!storeForPortLocked(port))
                {
                    result = kMcResultNoCard;
                }
                else
                {
                    McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                    cardType = kMcTypePs2;
                    freeBlocks = state.formatted ? kMcFreeClusters : 0;
                    format = state.formatted ? kMcFormatted : kMcUnformatted;
                    result = state.formatted ? kMcResultSucceed : kMcResultNoFormat;
                }
            }

            setMcCommandResultLocked(kMcCmdGetInfo, result);
        }

        if (typePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, typePtr))
            {
                std::memcpy(out, &cardType, sizeof(cardType));
            }
        }
        if (freePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, freePtr))
            {
                std::memcpy(out, &freeBlocks, sizeof(freeBlocks));
            }
        }
        if (formatPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, formatPtr))
            {
                std::memcpy(out, &format, sizeof(format));
            }
        }

        RUNTIME_LOG("[MC] GetInfo port=" << port << " type=" << cardType
                                         << " free=" << freeBlocks << " format=" << format
                                         << " result=" << result);
        setReturnS32(ctx, 0);
    }

    void sceMcGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceMcInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
                state.formatted = true;
            }
        }
        setReturnS32(ctx, 0);
    }

    void sceMcMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            result = withFormattedCardLocked(port, slot, [&](MemoryCardStore &store, McPortState &)
                                             { return store.makeDirectory(normalizeGuestMcPathLocked(port, path)); });
            setMcCommandResultLocked(kMcCmdMkdir, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const uint32_t flags = getRegU32(ctx, 7);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            result = withFormattedCardLocked(port, slot, [&](MemoryCardStore &store, McPortState &)
                                             {
                if (g_mcFiles.size() >= kMcMaxOpenFiles)
                {
                    return kMcResultUpLimitHandle;
                }
                const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                uint64_t handle = 0;
                const int32_t opened = store.open(guestPath, flags, handle);
                if (opened < 0)
                {
                    return opened;
                }
                const int32_t fd = allocateMcFdLocked(port, handle, store.describe(guestPath));
                if (fd < 0)
                {
                    store.close(handle);
                }
                return fd; });
            setMcCommandResultLocked(kMcCmdOpen, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t dstAddr = getRegU32(ctx, 5);
        const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
        uint8_t *dst = (size > 0) ? getMemPtr(rdram, dstAddr) : nullptr;

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            McOpenFile *openFile = nullptr;
            MemoryCardStore *store = storeForFdLocked(fd, openFile);
            if (size <= 0)
            {
                result = 0;
            }
            else if (!store)
            {
                result = kMcResultNoEntry;
            }
            else if (!dst)
            {
                result = kMcResultDeniedPermit;
            }
            else
            {
                result = store->read(openFile->handle, dst, static_cast<uint32_t>(size));
            }

            setMcCommandResultLocked(kMcCmdRead, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcRename(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string oldPath = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);
        const std::string newPath = readPs2CStringBounded(rdram, getRegU32(ctx, 7), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            result = withFormattedCardLocked(port, slot, [&](MemoryCardStore &store, McPortState &)
                                             { return store.rename(normalizeGuestMcPathLocked(port, oldPath),
                                                                   normalizeGuestMcPathLocked(port, newPath)); });
            setMcCommandResultLocked(kMcCmdRename, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcSeek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t offset = static_cast<int32_t>(getRegU32(ctx, 5));
        const int32_t origin = static_cast<int32_t>(getRegU32(ctx, 6));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            McOpenFile *openFile = nullptr;
            if (MemoryCardStore *store = storeForFdLocked(fd, openFile))
            {
                result = store->seek(openFile->handle, offset, origin);
            }
            setMcCommandResultLocked(kMcCmdSeek, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcSetFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        const std::string path = readPs2CStringBounded(rdram, getRegU32(ctx, 6), kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            result = withFormattedCardLocked(port, slot, [&](MemoryCardStore &store, McPortState &)
                                             { return store.exists(normalizeGuestMcPathLocked(port, path)) ? kMcResultSucceed
                                                                                                            : kMcResultNoEntry; });
            setMcCommandResultLocked(kMcCmdSetFileInfo, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cmdPtr = getRegU32(ctx, 5);
        const uint32_t resultPtr = getRegU32(ctx, 6);
        int32_t cmd = 0;
        int32_t result = 0;
        bool hadPending = false;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            hadPending = g_mcCommandPending;
            g_mcCommandPending = false;
            cmd = g_mcLastCmd;
            result = g_mcLastResult;
        }

        // libmc semantics: -1 means no async operation was executing; games rely
        // on it to tell idle polling apart from command completion.
        if (!hadPending)
        {
            setReturnS32(ctx, -1);
            return;
        }

        RUNTIME_LOG("[MC] Sync cmd=" << cmd << " result=" << result);

        if (cmdPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, cmdPtr))
            {
                std::memcpy(out, &cmd, sizeof(cmd));
            }
        }
        if (resultPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, resultPtr))
            {
                std::memcpy(out, &result, sizeof(result));
            }
        }

        // 1 = command finished in this runtime's immediate model.
        setReturnS32(ctx, 1);
    }

    void sceMcUnformat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t port = static_cast<int32_t>(getRegU32(ctx, 4));
        const int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                MemoryCardStore *store = storeForPortLocked(port);
                if (!store)
                {
                    result = kMcResultNoCard;
                }
                else
                {
                    closeMcFilesForPortLocked(port);
                    result = store->erase();
                    if (result == kMcResultSucceed)
                    {
                        McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                        state.currentDir = "/";
                        state.formatted = false;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdUnformat, result);
        }
        setReturnS32(ctx, 0);
    }

    void sceMcWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
        const uint8_t *src = (size > 0) ? getConstMemPtr(rdram, srcAddr) : nullptr;

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            McOpenFile *openFile = nullptr;
            MemoryCardStore *store = storeForFdLocked(fd, openFile);
            if (size <= 0)
            {
                result = 0;
            }
            else if (!store)
            {
                result = kMcResultNoEntry;
            }
            else if (!src)
            {
                result = kMcResultDeniedPermit;
            }
            else
            {
                result = store->write(openFile->handle, src, static_cast<uint32_t>(size));
            }

            setMcCommandResultLocked(kMcCmdWrite, result);
        }
        setReturnS32(ctx, 0);
    }

    void mcCallMessageTypeSe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcCheckReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateConfigInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateIconInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateSaveFileInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcDispFileName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispFileNumber(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplayFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfoMesCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowCurSol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowFoundtion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceGetInfoApdx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceIntrReadFixAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceStorePwd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetConfigCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcConfigFileBytes);
    }

    void mcGetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, g_cvMcFileCursor);
    }

    void mcGetFreeCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcRequiredFreeKb);
    }

    void mcGetIconCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconInfoBytes);
    }

    void mcGetIconFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconFileBytes);
    }

    void mcGetPortSelectDirInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetSaveFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcSaveFileBytes);
    }

    void mcGetStringEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t strAddr = getRegU32(ctx, 4);
        const std::string value = readPs2CStringBounded(rdram, runtime, strAddr, 1024);
        setReturnU32(ctx, strAddr + static_cast<uint32_t>(value.size()));
    }

    void mcMoveFileSelectWindowCursor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t delta = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor += delta;
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcNewCreateConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateIcon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSelectFileInfoInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 1);
    }

    void mcSelectSaveFileCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcSetFileSelectWindowCursolInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 0);
    }

    void mcSetStringSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcSetTyepWriteMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcWriteIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }
}
