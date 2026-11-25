// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/cia_rom.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>
#include "common/file_util.h"

namespace FileUtil {
namespace {
constexpr u64 Align64(u64 n) {
    return (n + 63) & ~u64(63);
}

u64 Little(const u8* p, unsigned bytes) {
    u64 result = 0;
    for (unsigned i = 0; i < bytes; ++i)
        result |= u64(p[i]) << (8 * i);
    return result;
}

u64 Big(const u8* p, unsigned bytes) {
    u64 result = 0;
    for (unsigned i = 0; i < bytes; ++i)
        result = (result << 8) | p[i];
    return result;
}

u64 SignedBodyOffset(const u8* signature) {
    switch (Big(signature, 4)) {
    case 0x10000:
    case 0x10003:
        return 0x240;
    case 0x10001:
    case 0x10004:
        return 0x140;
    case 0x10002:
    case 0x10005:
        return 0x40;
    default:
        return 0;
    }
}

struct Region {
    u64 offset, length;
};

struct Package {
    u64 title;
    Region main;
    bool encrypted;
};

template <typename Read>
std::optional<Package> ReadPackage(u64 physical, Read read) {
    if (physical > u64(std::numeric_limits<s64>::max()))
        return {};
    // CIA header, signed TMD body and content-chunk layouts correspond to
    // FileSys::CIAContainer and FileSys::TitleMetadata in the installer.
    std::array<u8, 0x2020> header;
    if (!read(0, header) || Little(header.data(), 4) != header.size() ||
        Little(header.data() + 4, 4) != 0)
        return {};
    const auto ticket = Align64(Align64(header.size()) + Little(&header[8], 4));
    const auto tmd = Align64(ticket + Little(&header[12], 4));
    const auto tmd_size = Little(&header[16], 4);
    // Cap signed metadata reads, including the installer’s subsequent ticket and TMD reads.
    if (Little(&header[8], 4) > (1 << 20) || Little(&header[12], 4) > (1 << 20) ||
        tmd_size > 0x240 + 0x9c4 + 0xffff * 0x30)
        return {};
    const auto data = Align64(tmd + tmd_size);
    const auto total = Little(&header[24], 8);
    if (data > physical || total > physical - data)
        return {};
    const auto meta_size = Little(&header[20], 4);
    if (meta_size && (meta_size < 0x400 || Align64(data + total) > physical ||
                      meta_size > physical - Align64(data + total)))
        return {};

    std::array<u8, 4> signature;
    if (!read(ticket, signature))
        return {};
    const auto ticket_body = SignedBodyOffset(signature.data());
    std::array<u8, 8> ticket_title;
    if (!ticket_body || ticket_body + 0x210 > Little(&header[12], 4) ||
        !read(ticket + ticket_body + 0x9c, ticket_title) || !read(tmd, signature))
        return {};
    const auto body_offset = SignedBodyOffset(signature.data());
    if (!body_offset)
        return {};
    std::array<u8, 0x9c4> body;
    if (tmd_size < body_offset + body.size() || !read(tmd + body_offset, body))
        return {};
    const auto title = Big(&body[0x4c], 8);
    const auto count = Big(&body[0x9e], 2);
    const auto boot_index = Big(&body[0xa0], 2);
    if (!title || title != Big(ticket_title.data(), 8) || count == 0 ||
        count * 0x30 > tmd_size - body_offset - body.size())
        return {};
    std::vector<u8> chunks(count * 0x30);
    if (!read(tmd + body_offset + body.size(), chunks))
        return {};
    std::vector<u32> update_ids;
    Region main{};
    bool encrypted = false;
    u64 consumed = 0;
    int previous_index = -1;
    for (unsigned i = 0; i < count; ++i) {
        const auto* chunk = chunks.data() + i * 0x30;
        const auto index = static_cast<unsigned>(Big(chunk + 4, 2));
        if (((title >> 32) == 0x0004000e && index != i) ||
            static_cast<int>(index) <= previous_index)
            return {};
        previous_index = static_cast<int>(index);
        if ((title >> 32) == 0x0004000e)
            update_ids.push_back(static_cast<u32>(Big(chunk, 4)));
        auto& present = header[0x20 + index / 8];
        const auto bit = 0x80u >> (index % 8);
        if (!(present & bit))
            continue;
        present &= ~bit;
        const u64 size = Big(chunk + 8, 8);
        if (!size || ((Big(chunk + 6, 2) & 1) && size % 16 != 0) || size > total - consumed)
            return {};
        if (index == boot_index) {
            encrypted = (Big(chunk + 6, 2) & 1) != 0;
            main = {data + consumed, size};
        }
        consumed += size;
    }
    std::sort(update_ids.begin(), update_ids.end());
    if (std::adjacent_find(update_ids.begin(), update_ids.end()) != update_ids.end())
        return {};
    if (!consumed || ((title >> 32) == 0x0004000e && !main.length) || consumed != total ||
        std::any_of(header.begin() + 0x20, header.end(), [](u8 mask) { return mask != 0; }))
        return {};
    return Package{title, main, encrypted};
}

std::optional<Region> MainContent(IOHandler& file) {
    const auto physical = file.GetSize();
    const auto read = [&](u64 offset, auto& buffer) {
        return offset <= physical && buffer.size() <= physical - offset &&
               file.Seek(static_cast<s64>(offset), SEEK_SET) &&
               file.Read(buffer.data(), 1, buffer.size()) == buffer.size();
    };
    const auto package = ReadPackage(physical, read);
    if (!package || (package->title >> 32) != 0x00040000 || package->encrypted)
        return {};
    const auto& main = package->main;
    std::array<u8, 0x200> ncch;
    if (main.length < ncch.size() || !read(main.offset, ncch) ||
        std::memcmp(ncch.data() + 0x100, "NCCH", 4) || !(ncch[0x18d] & 2) ||
        Little(&ncch[0x118], 8) != package->title || ncch[0x18e] != 0 ||
        Little(&ncch[0x104], 4) * 0x200 != main.length)
        return {};
    return main;
}

class CiaReader final : public IOHandler {
public:
    CiaReader(std::unique_ptr<IOHandler> file, Region region)
        : source(std::move(file)), region(region) {}
    std::size_t Read(void* buffer, std::size_t size, std::size_t count) override {
        if (!size || count > std::numeric_limits<std::size_t>::max() / size)
            return 0;
        const auto wanted = std::min<u64>(region.length - position, size * count);
        if (!wanted)
            return 0;
        const auto bytes = source->Read(buffer, 1, wanted);
        position += bytes;
        return bytes / size;
    }
    bool Seek(s64 offset, int whence) override {
        s64 origin = 0;
        switch (whence) {
        case SEEK_SET:
            break;
        case SEEK_CUR:
            origin = static_cast<s64>(position);
            break;
        case SEEK_END:
            origin = static_cast<s64>(region.length);
            break;
        default:
            return false;
        }
        if (offset < -origin || offset > static_cast<s64>(region.length) - origin)
            return false;
        const u64 next = origin + offset;
        if (!source->Seek(static_cast<s64>(region.offset + next), SEEK_SET))
            return false;
        position = next;
        return true;
    }
    u64 Tell() override {
        return position;
    }
    u64 GetSize() override {
        return region.length;
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
    std::unique_ptr<IOHandler> source;
    Region region;
    u64 position = 0;
};
} // namespace

static std::optional<Package> InspectPackage(const std::string& path) {
    IOFile file(path, "rb");
    if (!file.IsOpen())
        return {};
    const auto physical = file.GetSize();
    return ReadPackage(physical, [&](u64 offset, auto& buffer) {
        return offset <= physical && buffer.size() <= physical - offset &&
               file.Seek(static_cast<s64>(offset), SEEK_SET) &&
               file.ReadBytes(buffer.data(), buffer.size()) == buffer.size();
    });
}

std::optional<u64> GetCiaTitleId(const std::string& path) {
    const auto package = InspectPackage(path);
    return package ? std::optional<u64>{package->title} : std::nullopt;
}

bool IsEncryptedCia(const std::string& path) {
    const auto package = InspectPackage(path);
    return package && package->encrypted;
}

std::unique_ptr<IOHandler> OpenCiaRom(std::unique_ptr<IOHandler> source) {
    const auto region = MainContent(*source);
    if (!region || !source->Seek(static_cast<s64>(region->offset), SEEK_SET))
        return {};
    return std::make_unique<CiaReader>(std::move(source), *region);
}
} // namespace FileUtil
