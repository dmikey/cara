// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/sevenzip_rom.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <unistd.h>
#include "common/file_util.h"
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
    if (!name) name = archive_entry_pathname(entry);
    if (!name || archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry))
        return false;
    const std::filesystem::path path{name};
    for (const auto& part : path)
        if (part == "__MACOSX") return false;
    const auto extension = Common::ToLower(path.extension().string());
    return !path.filename().string().starts_with("._") && (extension == ".3ds" || extension == ".cci");
}

struct Cartridge {
    int fd = -1;
    u64 size = 0;
    ~Cartridge() { if (fd >= 0) close(fd); }
};

std::shared_ptr<Cartridge> Extract(IOHandler& file) {
    ArchiveSource source{file, {}};
    std::unique_ptr<archive, decltype(&archive_read_free)> reader{archive_read_new(), archive_read_free};
    if (!reader || archive_read_support_format_7zip(reader.get()) != ARCHIVE_OK ||
        archive_read_set_seek_callback(reader.get(), SeekSource) != ARCHIVE_OK ||
        archive_read_open(reader.get(), &source, nullptr, ReadSource, nullptr) != ARCHIVE_OK)
        return {};

    std::shared_ptr<Cartridge> cartridge;
    archive_entry* entry;
    int status;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        if (!IsCartridge(entry)) continue;
        const auto size = archive_entry_size(entry);
        if (cartridge || archive_entry_is_encrypted(entry) || !archive_entry_size_is_set(entry) ||
            size <= 0 || size > (8LL << 30))
            return {};
        std::error_code error;
        auto directory = std::filesystem::temp_directory_path(error);
        if (error) return {};
        const auto space = std::filesystem::space(directory, error);
        if (error || space.available < static_cast<u64>(size)) return {};
        auto path = (directory / "cara-cartridge-XXXXXX").string();
        cartridge = std::make_shared<Cartridge>();
        cartridge->fd = mkstemp(path.data());
        if (cartridge->fd < 0 || unlink(path.c_str()) != 0) return {};
        std::array<u8, 64 * 1024> block;
        la_ssize_t bytes;
        while ((bytes = archive_read_data(reader.get(), block.data(), block.size())) > 0) {
            if (bytes > size - static_cast<s64>(cartridge->size)) return {};
            ssize_t written = 0;
            while (written < bytes) {
                const auto n = write(cartridge->fd, block.data() + written, bytes - written);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return {};
                written += n;
            }
            cartridge->size += bytes;
        }
        if (bytes < 0 || cartridge->size != static_cast<u64>(size)) return {};
    }
    return status == ARCHIVE_EOF ? cartridge : nullptr;
}

class Reader final : public IOHandler {
public:
    explicit Reader(std::shared_ptr<Cartridge> cart) : cartridge(std::move(cart)) {}
    std::size_t Read(void* buffer, std::size_t size, std::size_t count) override {
        if (!size || count > std::numeric_limits<std::size_t>::max() / size) return 0;
        const auto bytes = std::min<u64>(size * count, cartridge->size - position);
        std::size_t done = 0;
        while (done < bytes) {
            const auto n = pread(cartridge->fd, static_cast<u8*>(buffer) + done,
                                 std::min<u64>(bytes - done, 1U << 30), position);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            position += n;
            done += n;
        }
        return done / size;
    }
    bool Seek(s64 offset, int origin) override {
        s64 base;
        switch (origin) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = position; break;
        case SEEK_END: base = cartridge->size; break;
        default: return false;
        }
        if (offset < -base || offset > static_cast<s64>(cartridge->size) - base) return false;
        position = base + offset;
        return true;
    }
    u64 Tell() override { return position; }
    u64 GetSize() override { return cartridge->size; }
    std::size_t Write(const void*, std::size_t, std::size_t) override { return 0; }
    bool Resize(u64) override { return false; }
    bool Flush() override { return true; }
private:
    std::shared_ptr<Cartridge> cartridge;
    u64 position = 0;
};
} // namespace

std::unique_ptr<IOHandler> OpenSevenZipRom(const std::string& path, std::unique_ptr<IOHandler> source) {
    static std::mutex mutex;
    static std::map<std::string, std::weak_ptr<Cartridge>> active;
    std::lock_guard lock{mutex};
    std::erase_if(active, [](const auto& item) { return item.second.expired(); });
    auto cartridge = active[path].lock();
    if (!cartridge) {
        cartridge = Extract(*source);
        if (!cartridge) return {};
        active[path] = cartridge;
    }
    return std::make_unique<Reader>(std::move(cartridge));
}
} // namespace FileUtil
