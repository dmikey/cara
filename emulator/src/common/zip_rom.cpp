// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/zip_rom.h"
#include "common/encrypted_title_zip.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <zlib.h>
#include "common/file_util.h"
#include "common/logging/log.h"

namespace FileUtil {
namespace {
constexpr u64 MiB = 1024 * 1024;
constexpr u64 MaxRomSize = 8 * 1024 * MiB;
constexpr std::size_t ChunkSize = 64 * 1024;

[[noreturn]] void Invalid(const char* reason) {
    throw std::runtime_error(reason);
}

u64 Little(std::span<const u8> bytes, std::size_t position, unsigned size) {
    if (position > bytes.size() || size > bytes.size() - position)
        Invalid("Truncated ZIP field");
    u64 value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= u64{bytes[position + i]} << (8 * i);
    return value;
}

bool EndsWith(std::string_view name, std::string_view suffix) {
    if (name.size() < suffix.size())
        return false;
    name.remove_prefix(name.size() - suffix.size());
    return std::equal(name.begin(), name.end(), suffix.begin(), [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == b;
    });
}

class ArchiveFile {
public:
    explicit ArchiveFile(std::unique_ptr<IOHandler> input_)
        : input(std::move(input_)), size(input->GetSize()) {}

    void ReadAt(u64 offset, std::span<u8> bytes) {
        if (offset > size || bytes.size() > size - offset ||
            offset > std::numeric_limits<s64>::max())
            Invalid("ZIP read outside archive");
        const std::lock_guard lock(mutex);
        if (!input->Seek(static_cast<s64>(offset), SEEK_SET) ||
            input->Read(bytes.data(), 1, bytes.size()) != bytes.size())
            Invalid("Cannot read ZIP archive");
    }

    const std::unique_ptr<IOHandler> input;
    const u64 size;
private:
    std::mutex mutex;
};

struct Directory {
    u64 offset;
    u64 size;
    u64 entries;
};

Directory ReadDirectory(ArchiveFile& file) {
    const u64 tail_size = std::min(file.size, u64{22 + 65535});
    std::vector<u8> tail(static_cast<std::size_t>(tail_size));
    file.ReadAt(file.size - tail_size, tail);
    for (std::size_t end = tail.size(); end >= 22; --end) {
        const auto record = std::span(tail).subspan(end - 22);
        if (Little(record, 0, 4) != 0x06054b50 || Little(record, 20, 2) != record.size() - 22)
            continue;
        if (Little(record, 4, 4) != 0 || Little(record, 8, 2) != Little(record, 10, 2))
            Invalid("Multi-disk ZIP archives are unsupported");
        Directory directory{Little(record, 16, 4), Little(record, 12, 4), Little(record, 10, 2)};
        u64 directory_end = file.size - tail_size + end - 22;
        const bool zip64_required = directory.offset == 0xffffffff ||
                                    directory.size == 0xffffffff || directory.entries == 0xffff;
        if (zip64_required || directory.offset + directory.size != directory_end) {
            if (directory_end < 20)
                Invalid("Missing ZIP64 locator");
            std::array<u8, 20> locator{};
            file.ReadAt(directory_end - locator.size(), locator);
            if (Little(locator, 0, 4) != 0x07064b50 || Little(locator, 4, 4) ||
                Little(locator, 16, 4) != 1)
                Invalid("Invalid ZIP64 locator");
            const u64 zip64 = Little(locator, 8, 8);
            std::array<u8, 56> header{};
            if (zip64 > directory_end - locator.size() ||
                header.size() > directory_end - locator.size() - zip64)
                Invalid("ZIP64 directory header outside archive");
            file.ReadAt(zip64, header);
            if (Little(header, 0, 4) != 0x06064b50 || Little(header, 4, 8) < 44 ||
                Little(header, 4, 8) != directory_end - locator.size() - zip64 - 12 ||
                Little(header, 16, 8) != 0 || Little(header, 24, 8) != Little(header, 32, 8))
                Invalid("Invalid ZIP64 directory");
            directory = {Little(header, 48, 8), Little(header, 40, 8), Little(header, 32, 8)};
            directory_end = zip64;
        }
        if (directory.offset > directory_end || directory.size != directory_end - directory.offset ||
            directory.entries > 65536 || directory.entries > directory.size / 46)
            Invalid("ZIP central directory outside archive");
        return directory;
    }
    Invalid("ZIP end record missing");
}

struct Member {
    u64 data_offset;
    u64 packed_size;
    u64 size;
    u32 crc;
    u16 method;
};

Member FindRom(ArchiveFile& file) {
    const auto directory = ReadDirectory(file);
    const u64 end = directory.offset + directory.size;
    u64 cursor = directory.offset;
    std::optional<Member> result;
    for (u64 entry = 0; entry < directory.entries; ++entry) {
        std::array<u8, 46> record{};
        if (record.size() > end - cursor)
            Invalid("Truncated ZIP directory entry");
        file.ReadAt(cursor, record);
        if (Little(record, 0, 4) != 0x02014b50)
            Invalid("Invalid ZIP directory entry");
        cursor += record.size();
        const auto name_size = Little(record, 28, 2);
        const auto extra_size = Little(record, 30, 2);
        const auto comment_size = Little(record, 32, 2);
        if (name_size + extra_size + comment_size > end - cursor)
            Invalid("ZIP directory entry exceeds its bounds");
        std::vector<u8> name_bytes(static_cast<std::size_t>(name_size));
        file.ReadAt(cursor, name_bytes);
        cursor += name_size;
        const std::string_view name{reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size()};
        const bool is_rom = EndsWith(name, ".3ds") && !name.starts_with("__MACOSX/") &&
                            !name.substr(name.find_last_of('/') + 1).starts_with("._");
        if (!is_rom) {
            cursor += extra_size + comment_size;
            continue;
        }
        if (result || name.find('\0') != std::string_view::npos)
            Invalid("ZIP must contain one unambiguous .3ds member");
        const u64 flags = Little(record, 8, 2);
        const auto method = static_cast<u16>(Little(record, 10, 2));
        if ((flags & ~0x080eu) || (method != 0 && method != Z_DEFLATED))
            Invalid("ZIP ROM must use Stored or Deflate without encryption");
        u64 packed = Little(record, 20, 4);
        u64 size = Little(record, 24, 4);
        u64 local_offset = Little(record, 42, 4);
        u64 disk = Little(record, 34, 2);
        if (packed == 0xffffffff || size == 0xffffffff || local_offset == 0xffffffff ||
            disk == 0xffff) {
            std::vector<u8> extras(static_cast<std::size_t>(extra_size));
            file.ReadAt(cursor, extras);
            std::span<const u8> fields{extras};
            bool found = false;
            while (!fields.empty()) {
                const auto length = Little(fields, 2, 2);
                if (length > fields.size() - 4)
                    Invalid("Invalid ZIP extra field");
                if (Little(fields, 0, 2) == 1) {
                    auto field = fields.subspan(4, length);
                    const auto next = [&](unsigned bytes) {
                        const auto value = Little(field, 0, bytes);
                        field = field.subspan(bytes);
                        return value;
                    };
                    if (size == 0xffffffff) size = next(8);
                    if (packed == 0xffffffff) packed = next(8);
                    if (local_offset == 0xffffffff) local_offset = next(8);
                    if (disk == 0xffff) disk = next(4);
                    found = true;
                    break;
                }
                fields = fields.subspan(4 + length);
            }
            if (!found)
                Invalid("Missing ZIP64 entry sizes");
        }
        cursor += extra_size + comment_size;
        std::array<u8, 30> local{};
        if (disk || !size || size > MaxRomSize || !packed ||
            (method == 0 && packed != size) || local_offset > directory.offset ||
            local.size() > directory.offset - local_offset)
            Invalid("Invalid ZIP ROM size or offset");
        file.ReadAt(local_offset, local);
        if (Little(local, 0, 4) != 0x04034b50 || Little(local, 6, 2) != flags ||
            Little(local, 8, 2) != method || Little(local, 26, 2) != name_size)
            Invalid("ZIP local header differs from central directory");
        const u64 name_offset = local_offset + local.size();
        const u64 data = name_offset + name_size + Little(local, 28, 2);
        if (data > directory.offset || packed > directory.offset - data)
            Invalid("ZIP ROM extends into central directory");
        std::vector<u8> local_name(name_bytes.size());
        file.ReadAt(name_offset, local_name);
        if (local_name != name_bytes)
            Invalid("ZIP local member name differs");
        result = Member{data, packed, size, static_cast<u32>(Little(record, 16, 4)), method};
    }
    if (cursor != end || !result)
        Invalid("No standalone .3ds member in ZIP directory");
    return *result;
}

struct Inflater {
    z_stream stream{};
    bool active = false;
    ~Inflater() {
        if (active)
            inflateEnd(&stream);
    }
    void Reset(z_stream* source) {
        if (active)
            inflateEnd(&stream);
        stream = {};
        active = (source ? inflateCopy(&stream, source) : inflateInit2(&stream, -MAX_WBITS)) == Z_OK;
        if (!active)
            Invalid("Cannot allocate ZIP inflater");
    }
};

class Decoder {
public:
    Decoder(ArchiveFile& file_, const Member& member_) : file(file_), member(member_) {}
    void Restart(z_stream* source, u64 offset) {
        inflater.Reset(source);
        next_input = offset;
        ended = false;
    }
    std::size_t Read(std::span<u8> output) {
        auto& s = inflater.stream;
        std::size_t written = 0;
        while (!output.empty() && !ended) {
            if (!s.avail_in && next_input < member.packed_size) {
                const auto chunk = std::min<u64>(input.size(), member.packed_size - next_input);
                file.ReadAt(member.data_offset + next_input, std::span(input).first(chunk));
                next_input += chunk;
                s.next_in = input.data();
                s.avail_in = static_cast<uInt>(chunk);
            }
            const auto wanted = static_cast<uInt>(std::min<std::size_t>(output.size(), ChunkSize));
            s.next_out = output.data();
            s.avail_out = wanted;
            const auto before = s.avail_in;
            const int status = inflate(&s, Z_NO_FLUSH);
            const uInt produced = wanted - s.avail_out;
            output = output.subspan(produced);
            written += produced;
            if (status == Z_STREAM_END)
                ended = true;
            else if (status != Z_OK || (!produced && before == s.avail_in))
                Invalid("Invalid or truncated Deflate stream");
        }
        return written;
    }
    u64 CompressedPosition() const { return next_input - inflater.stream.avail_in; }

    Inflater inflater;
    bool ended = false;
private:
    ArchiveFile& file;
    const Member& member;
    u64 next_input = 0;
    std::array<u8, ChunkSize> input{};
};

struct Checkpoint {
    u64 compressed_offset;
    std::unique_ptr<Inflater> inflater;
};

struct Index {
    explicit Index(std::unique_ptr<IOHandler> input)
        : file(std::move(input)), member(FindRom(file)),
          spacing(std::max(MiB, (member.size + 1023) / 1024)) {
        std::array<u8, ChunkSize> bytes{};
        uLong crc = crc32(0, nullptr, 0);
        Decoder decoder(file, member);
        if (member.method == Z_DEFLATED)
            decoder.Restart(nullptr, 0);
        for (u64 position = 0; position < member.size;) {
            if (member.method == Z_DEFLATED && position % spacing == 0) {
                auto snapshot = std::make_unique<Inflater>();
                snapshot->Reset(&decoder.inflater.stream);
                // Retain the decoder state, including pending output and dictionary.
                // Refill its external input buffer from the stored compressed offset.
                snapshot->stream.next_in = Z_NULL;
                snapshot->stream.avail_in = 0;
                snapshot->stream.next_out = Z_NULL;
                snapshot->stream.avail_out = 0;
                checkpoints.push_back({decoder.CompressedPosition(), std::move(snapshot)});
            }
            const auto chunk = std::min({u64{bytes.size()}, member.size - position,
                                         spacing - position % spacing});
            const auto output = std::span(bytes).first(chunk);
            if (member.method == 0)
                file.ReadAt(member.data_offset + position, output);
            else if (decoder.Read(output) != chunk)
                Invalid("Deflate stream ended before the declared ROM size");
            crc = crc32(crc, bytes.data(), static_cast<uInt>(chunk));
            position += chunk;
        }
        if (member.method == Z_DEFLATED &&
            (decoder.Read(std::span(bytes).first(1)) != 0 || !decoder.ended ||
             decoder.CompressedPosition() != member.packed_size))
            Invalid("Deflate stream exceeds the declared ROM extent");
        if (crc != member.crc)
            Invalid("ZIP ROM CRC mismatch");
    }

    ArchiveFile file;
    const Member member;
    const u64 spacing;
    std::vector<Checkpoint> checkpoints;
    std::mutex snapshots;
};

class ZipRomHandler final : public IOHandler {
public:
    explicit ZipRomHandler(std::shared_ptr<Index> index_)
        : index(std::move(index_)), decoder(index->file, index->member) {}

    std::size_t Read(void* buffer, std::size_t size, std::size_t count) override {
        if (!size || !count || position >= index->member.size ||
            count > std::numeric_limits<std::size_t>::max() / size || failed)
            return 0;
        const auto length = static_cast<std::size_t>(
            std::min<u64>(size * count, index->member.size - position));
        std::span output{static_cast<u8*>(buffer), length};
        try {
            if (index->member.method == 0) {
                index->file.ReadAt(index->member.data_offset + position, output);
            } else {
                PositionDecoder();
                if (decoder.Read(output) != length)
                    Invalid("Deflate stream ended during indexed read");
                decoded += length;
            }
            position += length;
            return length / size;
        } catch (const std::exception& error) {
            failed = true;
            LOG_ERROR(Common_Filesystem, "Indexed ZIP ROM read failed: {}", error.what());
            return 0;
        }
    }

    bool Seek(s64 offset, int whence) override {
        u64 base = 0;
        switch (whence) {
        case SEEK_SET: break;
        case SEEK_CUR: base = position; break;
        case SEEK_END: base = index->member.size; break;
        default: return false;
        }
        if (offset < 0) {
            const u64 magnitude = static_cast<u64>(-(offset + 1)) + 1;
            if (magnitude > base)
                return false;
            position = base - magnitude;
        } else {
            if (static_cast<u64>(offset) > std::numeric_limits<s64>::max() - base)
                return false;
            position = base + static_cast<u64>(offset);
        }
        return true;
    }
    u64 Tell() override { return position; }
    u64 GetSize() override { return index->member.size; }
    std::size_t Write(const void*, std::size_t, std::size_t) override { return 0; }
    bool Resize(u64) override { return false; }
    bool Flush() override { return true; }

private:
    void PositionDecoder() {
        const auto point = position / index->spacing;
        const auto start = point * index->spacing;
        if (!decoder.inflater.active || decoded > position || decoded < start) {
            const auto& checkpoint = index->checkpoints.at(point);
            const std::lock_guard lock(index->snapshots);
            decoder.Restart(&checkpoint.inflater->stream, checkpoint.compressed_offset);
            decoded = start;
        }
        std::array<u8, ChunkSize> scratch;
        while (decoded < position) {
            const auto length = std::min<u64>(scratch.size(), position - decoded);
            if (decoder.Read(std::span(scratch).first(length)) != length)
                Invalid("Deflate stream ended during indexed seek");
            decoded += length;
        }
    }

    const std::shared_ptr<Index> index;
    Decoder decoder;
    u64 position = 0;
    u64 decoded = 0;
    bool failed = false;
};

} // namespace

bool IsEncryptedTitleZip(const std::string& path) {
    return HasEncryptedTitleZip(path);
}

bool IsZipRomPath(const std::string& path) {
    return EndsWith(path, ".zip");
}

std::unique_ptr<IOHandler> OpenIndexedZipRom(const std::string& path,
                                             std::unique_ptr<IOHandler> source) {
    static std::mutex cache_mutex;
    static std::map<std::string, std::weak_ptr<Index>> cache;
    const std::lock_guard lock(cache_mutex);
    if (const auto it = cache.find(path); it != cache.end()) {
        if (const auto existing = it->second.lock())
            return std::make_unique<ZipRomHandler>(existing);
    }
    std::erase_if(cache, [](const auto& entry) { return entry.second.expired(); });
    try {
        const auto start = std::chrono::steady_clock::now();
        auto index = std::make_shared<Index>(std::move(source));
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
        LOG_INFO(Common_Filesystem, "Indexed ZIP ROM {} bytes, {} checkpoints, {:.3f}s",
                 index->member.size, index->checkpoints.size(), elapsed.count());
        cache.emplace(path, index);
        return std::make_unique<ZipRomHandler>(std::move(index));
    } catch (const std::exception& error) {
        LOG_ERROR(Common_Filesystem, "Cannot open indexed ZIP ROM: {}", error.what());
        return {};
    }
}
} // namespace FileUtil
