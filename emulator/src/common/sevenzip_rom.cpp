// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/sevenzip_rom.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <archive.h>
#include <archive_entry.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/sevenzip_lzma2.h"
#include "common/string_util.h"

namespace FileUtil {
namespace {
struct ArchiveSource {
    IOHandler& file;
    std::array<u8, 64 * 1024> buffer;
};

la_ssize_t ReadSource(archive*, void* opaque, const void** buffer) {
    auto& source = *static_cast<ArchiveSource*>(opaque);
    *buffer = source.buffer.data();
    return source.file.Read(source.buffer.data(), 1, source.buffer.size());
}

la_int64_t SeekSource(archive*, void* opaque, la_int64_t offset, int origin) {
    auto& file = static_cast<ArchiveSource*>(opaque)->file;
    return file.Seek(offset, origin) ? static_cast<la_int64_t>(file.Tell()) : ARCHIVE_FATAL;
}

bool IsCartridge(archive_entry* entry) {
    const char* name = archive_entry_pathname_utf8(entry);
    if (!name)
        name = archive_entry_pathname(entry);
    if (!name || archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry))
        return false;
    const std::filesystem::path path{name};
    for (const auto& part : path)
        if (part == "__MACOSX")
            return false;
    const auto extension = Common::ToLower(path.extension().string());
    return !path.filename().string().starts_with("._") &&
           (extension == ".3ds" || extension == ".cci");
}

thread_local const SevenZipLoadScope* scope = nullptr;

struct Cartridge final : SevenZipCartridge {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::mutex read_mutex;

    ~Cartridge() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    bool OpenTemporary(const std::filesystem::path& directory) {
        wchar_t path[MAX_PATH]{};
        if (!GetTempFileNameW(directory.c_str(), L"car", 0, path))
            return false;
        handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                             nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            DeleteFileW(path);
            return false;
        }
        return true;
    }
    std::size_t ReadAt(u64 position, void* buffer, std::size_t bytes) override {
        std::lock_guard lock(read_mutex);
        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(position);
        if (!SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN))
            return 0;
        std::size_t done = 0;
        while (done < bytes) {
            DWORD count = 0;
            if (!ReadFile(handle, static_cast<u8*>(buffer) + done,
                          static_cast<DWORD>(std::min<std::size_t>(bytes - done, 1U << 30)),
                          &count, nullptr) || !count)
                break;
            done += count;
        }
        return done;
    }
    bool WriteBlock(const u8* buffer, std::size_t bytes) {
        DWORD written = 0;
        return WriteFile(handle, buffer, static_cast<DWORD>(bytes), &written, nullptr) &&
               written == bytes;
    }
#else
    int fd = -1;

    ~Cartridge() {
        if (fd >= 0)
            close(fd);
    }
    bool OpenTemporary(const std::filesystem::path& directory) {
        auto path = (directory / "cara-cartridge-XXXXXX").string();
        fd = mkstemp(path.data());
        return fd >= 0 && unlink(path.c_str()) == 0;
    }
    std::size_t ReadAt(u64 position, void* buffer, std::size_t bytes) override {
        std::size_t done = 0;
        while (done < bytes) {
            const auto n = pread(fd, static_cast<u8*>(buffer) + done,
                                 std::min<u64>(bytes - done, 1U << 30), position + done);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            done += n;
        }
        return done;
    }
    bool WriteBlock(const u8* buffer, std::size_t bytes) {
        std::size_t written = 0;
        while (written < bytes) {
            const auto n = write(fd, buffer + written, bytes - written);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return false;
            written += n;
        }
        return true;
    }
#endif
};

std::shared_ptr<SevenZipCartridge> Extract(std::unique_ptr<IOHandler> handle,
                                           const std::string& path, const std::string& directory,
                                           const SevenZipLoadScope::Progress& progress) {
    auto& file = *handle;
    ArchiveSource source{file, {}};
    std::unique_ptr<archive, decltype(&archive_read_free)> reader{archive_read_new(),
                                                                  archive_read_free};
    if (!reader || archive_read_support_format_7zip(reader.get()) != ARCHIVE_OK ||
        archive_read_set_seek_callback(reader.get(), SeekSource) != ARCHIVE_OK ||
        archive_read_open(reader.get(), &source, nullptr, ReadSource, nullptr) != ARCHIVE_OK)
        return {};

    std::shared_ptr<Cartridge> cartridge;
    std::optional<cara_lzma2_layout> demand;
    const auto start = std::chrono::steady_clock::now();
    archive_entry* entry;
    int status;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        if (!IsCartridge(entry))
            continue;
        const auto size = archive_entry_size(entry);
        if (cartridge || demand || archive_entry_is_encrypted(entry) ||
            !archive_entry_size_is_set(entry) || size <= 0 || size > (8LL << 30))
            return {};
        cara_lzma2_layout layout{};
        if (cara_7zip_lzma2_layout(reader.get(), &layout) && layout.properties <= 30) {
            demand = layout;
            continue; // List all remaining headers to reject a second cartridge.
        }
        if (progress)
            progress(0, size);
        u64 last_report = 0;
        std::error_code error;
        auto directory = std::filesystem::temp_directory_path(error);
        if (error)
            return {};
        const auto space = std::filesystem::space(directory, error);
        if (error || space.available < static_cast<u64>(size))
            return {};
        cartridge = std::make_shared<Cartridge>();
        if (!cartridge->OpenTemporary(directory))
            return {};
        std::array<u8, 64 * 1024> block;
        la_ssize_t bytes;
        while ((bytes = archive_read_data(reader.get(), block.data(), block.size())) > 0) {
            if (bytes > size - static_cast<s64>(cartridge->size))
                return {};
            if (!cartridge->WriteBlock(block.data(), bytes))
                return {};
            cartridge->size += bytes;
            if (progress && cartridge->size - last_report >= (4U << 20)) {
                progress(cartridge->size, size);
                last_report = cartridge->size;
            }
        }
        if (bytes < 0 || cartridge->size != static_cast<u64>(size))
            return {};
    }
    if (status != ARCHIVE_EOF)
        return {};
    reader.reset();
    if (demand)
        return OpenIndexedLzma2(std::move(handle), *demand, path, directory, progress);
    if (cartridge) {
        if (progress)
            progress(cartridge->size, cartridge->size);
        LOG_INFO(Common_Filesystem, "7z temporary extraction: {} bytes, {:.3f}s", cartridge->size,
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }
    return cartridge;
}

class Reader final : public IOHandler {
public:
    explicit Reader(std::shared_ptr<SevenZipCartridge> cart) : cartridge(std::move(cart)) {}
    std::size_t Read(void* buffer, std::size_t size, std::size_t count) override {
        if (!size || count > std::numeric_limits<std::size_t>::max() / size)
            return 0;
        const auto bytes = std::min<u64>(size * count, cartridge->size - position);
        const auto done = cartridge->ReadAt(position, buffer, bytes);
        position += done;
        return done / size;
    }
    bool Seek(s64 offset, int origin) override {
        s64 base;
        switch (origin) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = position;
            break;
        case SEEK_END:
            base = cartridge->size;
            break;
        default:
            return false;
        }
        if (offset < -base || offset > static_cast<s64>(cartridge->size) - base)
            return false;
        position = base + offset;
        return true;
    }
    u64 Tell() override {
        return position;
    }
    u64 GetSize() override {
        return cartridge->size;
    }
    std::size_t Write(const void*, std::size_t, std::size_t) override {
        return 0;
    }
    bool Resize(u64) override {
        return false;
    }
    bool Flush() override {
        return true;
    }

private:
    std::shared_ptr<SevenZipCartridge> cartridge;
    u64 position = 0;
};
} // namespace

SevenZipLoadScope::SevenZipLoadScope(std::string cache_directory, Progress progress)
    : directory(std::move(cache_directory)), report(std::move(progress)), previous(scope) {
    scope = this;
}
SevenZipLoadScope::~SevenZipLoadScope() {
    scope = previous;
}

std::unique_ptr<IOHandler> OpenSevenZipRom(const std::string& path,
                                           std::unique_ptr<IOHandler> source) {
    static std::mutex mutex;
    static std::map<std::string, std::weak_ptr<SevenZipCartridge>> active;
    std::lock_guard lock{mutex};
    std::erase_if(active, [](const auto& item) { return item.second.expired(); });
    const auto key = path + ":" + SevenZipSourceStamp(path, *source);
    auto cartridge = active[key].lock();
    if (!cartridge) {
        cartridge = Extract(std::move(source), path, scope ? scope->directory : "",
                            scope ? scope->report : SevenZipLoadScope::Progress{});
        if (!cartridge)
            return {};
        active[key] = cartridge;
    } else if (scope && scope->report) {
        scope->report(cartridge->size, cartridge->size);
    }
    return std::make_unique<Reader>(std::move(cartridge));
}
} // namespace FileUtil
