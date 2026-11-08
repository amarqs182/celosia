#include "runtime/ps2_vfs.h"

#include "runtime/ps2_memory.h"
#include "runtime/ps2_rom_device.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>

namespace
{
    class HostOpenFile final : public IPS2OpenFile
    {
    public:
        explicit HostOpenFile(FILE *file) : m_file(file) {}
        ~HostOpenFile() override
        {
            if (m_file)
                std::fclose(m_file);
        }

        int64_t read(void *destination, size_t size) override
        {
            if ((!destination && size != 0u) || !m_file)
                return -1;
            const size_t bytes = std::fread(destination, 1u, size, m_file);
            if (bytes < size && std::ferror(m_file))
            {
                std::clearerr(m_file);
                return -1;
            }
            return static_cast<int64_t>(bytes);
        }

        int64_t write(const void *source, size_t size) override
        {
            if ((!source && size != 0u) || !m_file)
                return -1;
            const size_t bytes = std::fwrite(source, 1u, size, m_file);
            if (bytes < size && std::ferror(m_file))
            {
                std::clearerr(m_file);
                return -1;
            }
            return static_cast<int64_t>(bytes);
        }

        int64_t seek(int64_t offset, int whence) override
        {
            if (!m_file || offset < std::numeric_limits<long>::min() || offset > std::numeric_limits<long>::max() || std::fseek(m_file, static_cast<long>(offset), whence) != 0)
                return -1;
            const long position = std::ftell(m_file);
            return position < 0 ? -1 : static_cast<int64_t>(position);
        }

    private:
        FILE *m_file = nullptr;
    };

    class MemoryOpenFile final : public IPS2OpenFile
    {
    public:
        explicit MemoryOpenFile(std::vector<uint8_t> bytes) : m_bytes(std::move(bytes)) {}

        int64_t read(void *destination, size_t size) override
        {
            if (!destination && size != 0u)
                return -1;
            const size_t available = m_position < m_bytes.size() ? m_bytes.size() - m_position : 0u;
            const size_t count = std::min(size, available);
            if (count != 0u)
                std::memcpy(destination, m_bytes.data() + m_position, count);
            m_position += count;
            return static_cast<int64_t>(count);
        }

        int64_t write(const void *, size_t) override
        {
            return -1;
        }

        int64_t seek(int64_t offset, int whence) override
        {
            int64_t base = 0;
            if (whence == SEEK_CUR)
                base = static_cast<int64_t>(m_position);
            else if (whence == SEEK_END)
                base = static_cast<int64_t>(m_bytes.size());
            else if (whence != SEEK_SET)
                return -1;

            const int64_t position = base + offset;
            if (position < 0 || static_cast<uint64_t>(position) > m_bytes.size())
                return -1;
            m_position = static_cast<size_t>(position);
            return position;
        }

    private:
        std::vector<uint8_t> m_bytes;
        size_t m_position = 0u;
    };

    const char *hostMode(uint32_t flags)
    {
        const bool read = (flags & PS2_FIO_O_RDONLY) != 0u || (flags & PS2_FIO_O_RDWR) == PS2_FIO_O_RDWR;
        const bool write = (flags & PS2_FIO_O_WRONLY) != 0u || (flags & PS2_FIO_O_RDWR) == PS2_FIO_O_RDWR;
        const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
        const bool truncate = (flags & PS2_FIO_O_TRUNC) != 0u;
        const bool append = (flags & PS2_FIO_O_APPEND) != 0u;

        if (read && write)
        {
            if (truncate)
                return "w+b";
            if (append)
                return "a+b";
            return "r+b";
        }
        if (write)
        {
            if (append)
                return "ab";
            if (create || truncate)
                return "wb";
            return "r+b";
        }
        return "rb";
    }

    bool safeRelativePath(std::string_view suffix, std::filesystem::path &relative)
    {
        relative = std::filesystem::path(suffix).lexically_normal();
        if (relative.is_absolute() || relative.has_root_name())
            return false;
        for (const auto &part : relative)
        {
            if (part == "..")
                return false;
        }
        return true;
    }

    std::time_t toTimeT(std::filesystem::file_time_type value)
    {
        const auto systemValue = std::chrono::time_point_cast<std::chrono::system_clock::duration>(value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        return std::chrono::system_clock::to_time_t(systemValue);
    }

    // Diagnostic-only VFS logging helpers (no behavior change).
    // Rate-limited to kVfsDiagLogMaxLines lines per run for the [vfs:host]
    // stream; paths containing "STARTUP" (case-insensitive) always bypass it.
    constexpr int kVfsDiagLogMaxLines = 200;
    std::atomic<int> g_vfsDiagLogCount{0};

    bool vfsDiagPathIsStartup(std::string_view path)
    {
        static constexpr std::string_view kNeedle = "startup";
        if (path.size() < kNeedle.size())
            return false;
        for (size_t i = 0; i + kNeedle.size() <= path.size(); ++i)
        {
            bool match = true;
            for (size_t j = 0; j < kNeedle.size(); ++j)
            {
                const char a = path[i + j];
                const char b = kNeedle[j];
                const char ua = (a >= 'a' && a <= 'z') ? static_cast<char>(a - 'a' + 'A') : a;
                const char ub = (b >= 'a' && b <= 'z') ? static_cast<char>(b - 'a' + 'A') : b;
                if (ua != ub)
                {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
        return false;
    }

    bool vfsDiagLogAllowed(std::string_view ps2Path, std::string_view hostPath)
    {
        if (vfsDiagPathIsStartup(ps2Path) || vfsDiagPathIsStartup(hostPath))
            return true;
        return g_vfsDiagLogCount.fetch_add(1, std::memory_order_relaxed) < kVfsDiagLogMaxLines;
    }

    void logVfsHost(const char *op, std::string_view ps2Path, const std::filesystem::path &hostPath, bool exists, std::string_view errText)
    {
        const std::string hostStr = hostPath.string();
        if (!vfsDiagLogAllowed(ps2Path, hostStr))
            return;
        std::cerr << "[vfs:host] op=" << op << " path=\"" << ps2Path << "\" host=\"" << hostStr
                  << "\" exists=" << (exists ? 1 : 0) << " err=\"" << errText << "\"" << std::endl;
    }
}

PS2Vfs::~PS2Vfs() = default;

int32_t PS2Vfs::open(std::string_view path, uint32_t flags, const PS2VfsMounts &mounts, const PS2RomDevice &rom)
{
    const ps2x::iop::ParsedPs2Path parsed = ps2x::iop::parsePs2Path(path);
    if (!parsed)
        return -1;

    std::unique_ptr<IPS2OpenFile> file;
    if (parsed.device == ps2x::iop::Ps2PathDevice::Rom0)
    {
        const uint32_t access = flags & PS2_FIO_O_RDWR;
        if (access != PS2_FIO_O_RDONLY || (flags & (PS2_FIO_O_CREAT | PS2_FIO_O_TRUNC)) != 0u)
            return -1;
        std::vector<uint8_t> bytes;
        if (!rom.readFile(parsed.path, bytes))
            return -1;
        file = std::make_unique<MemoryOpenFile>(std::move(bytes));
    }
    else
    {
        std::filesystem::path hostPath;
        if (!resolveHostPath(path, mounts, hostPath))
        {
            // Diagnostic only (no behavior change): log the failed resolution.
            logVfsHost("open", path, hostPath, false, "resolve failed");
            return -1;
        }

        std::error_code existsError;
        const bool exists = std::filesystem::exists(hostPath, existsError);
        if (existsError || !exists)
        {
            // Diagnostic only: report missing/erroring host paths.
            logVfsHost("open", path, hostPath, exists, existsError ? existsError.message() : "not found");
        }
        if (existsError || (exists && (flags & (PS2_FIO_O_CREAT | PS2_FIO_O_EXCL)) == (PS2_FIO_O_CREAT | PS2_FIO_O_EXCL)))
        {
            return -1;
        }

        FILE *stream = std::fopen(hostPath.string().c_str(), hostMode(flags));
        int openErrno = errno;
        const uint32_t access = flags & PS2_FIO_O_RDWR;
        if (!stream && !exists && (flags & PS2_FIO_O_CREAT) != 0u &&
            access == PS2_FIO_O_RDWR &&
            (flags & (PS2_FIO_O_TRUNC | PS2_FIO_O_APPEND)) == 0u)
        {
            stream = std::fopen(hostPath.string().c_str(), "w+b");
            openErrno = errno;
        }
        if (!stream)
        {
            // Diagnostic only: report host-level open failures.
            logVfsHost("open", path, hostPath, exists, std::strerror(openErrno));
            return -1;
        }
        file = std::make_unique<HostOpenFile>(stream);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_nextDescriptor < 3)
        m_nextDescriptor = 3;
    const int32_t descriptor = m_nextDescriptor++;
    m_descriptors.emplace(descriptor, OpenDescriptor{std::move(file), parsed.deviceName, std::string(path)});
    return descriptor;
}

int32_t PS2Vfs::close(int32_t descriptor)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_descriptors.erase(descriptor) == 1u ? 0 : -1;
}

int64_t PS2Vfs::read(int32_t descriptor, void *destination, size_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_descriptors.find(descriptor);
    return found == m_descriptors.end() ? -1 : found->second.file->read(destination, size);
}

int64_t PS2Vfs::write(int32_t descriptor, const void *source, size_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_descriptors.find(descriptor);
    return found == m_descriptors.end() ? -1 : found->second.file->write(source, size);
}

int64_t PS2Vfs::seek(int32_t descriptor, int64_t offset, int whence)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_descriptors.find(descriptor);
    return found == m_descriptors.end() ? -1 : found->second.file->seek(offset, whence);
}

bool PS2Vfs::stat(std::string_view path, const PS2VfsMounts &mounts, const PS2RomDevice &rom, PS2VfsStat &result) const
{
    result = {};
    const ps2x::iop::ParsedPs2Path parsed = ps2x::iop::parsePs2Path(path);
    if (!parsed)
        return false;
    if (parsed.device == ps2x::iop::Ps2PathDevice::Rom0)
    {
        if (!rom.fileSize(parsed.path, result.size))
            return false;
        result.readOnly = true;
        return true;
    }

    std::filesystem::path hostPath;
    if (!resolveHostPath(path, mounts, hostPath))
    {
        // Diagnostic only (no behavior change): log the failed resolution.
        logVfsHost("stat", path, hostPath, false, "resolve failed");
        return false;
    }
    std::error_code error;
    const auto status = std::filesystem::status(hostPath, error);
    if (error || !std::filesystem::exists(status))
    {
        // Diagnostic only: report missing/erroring host paths.
        logVfsHost("stat", path, hostPath, std::filesystem::exists(status), error ? error.message() : "not found");
        return false;
    }
    result.directory = std::filesystem::is_directory(status);
    if (!result.directory)
    {
        result.size = std::filesystem::file_size(hostPath, error);
        if (error)
        {
            // Diagnostic only: report host-level stat failures.
            logVfsHost("stat", path, hostPath, true, error.message());
            return false;
        }
    }
    const auto modified = std::filesystem::last_write_time(hostPath, error);
    if (!error)
        result.created = result.accessed = result.modified = toTimeT(modified);
    result.readOnly = (status.permissions() & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
    return true;
}

bool PS2Vfs::resolveHostPath(std::string_view path, const PS2VfsMounts &mounts, std::filesystem::path &result) const
{
    result.clear();
    const ps2x::iop::ParsedPs2Path parsed = ps2x::iop::parsePs2Path(path);
    if (!parsed || parsed.device == ps2x::iop::Ps2PathDevice::Rom0)
        return false;
    if (parsed.device == ps2x::iop::Ps2PathDevice::NativeHost)
    {
        result = std::filesystem::path(parsed.path).lexically_normal();
        return !result.empty();
    }

    std::filesystem::path base;
    switch (parsed.device)
    {
    case ps2x::iop::Ps2PathDevice::Host:
        base = mounts.hostRoot;
        break;
    case ps2x::iop::Ps2PathDevice::Cdrom:
        base = mounts.cdRoot;
        break;
    case ps2x::iop::Ps2PathDevice::MemoryCard0:
        base = mounts.memoryCard0Root;
        break;
    default:
        return false;
    }
    std::filesystem::path relative;
    if (base.empty() || !safeRelativePath(parsed.path, relative))
        return false;
    result = (base / relative).lexically_normal();
    return true;
}

std::vector<PS2VfsDescriptorInfo> PS2Vfs::descriptors() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PS2VfsDescriptorInfo> result;
    result.reserve(m_descriptors.size());
    for (const auto &[descriptor, entry] : m_descriptors)
        result.push_back({descriptor, entry.device, entry.path});
    return result;
}
