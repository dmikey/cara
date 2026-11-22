// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/z3ds_rom.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <zstd_seekable.h>
#include "common/file_util.h"

namespace FileUtil {
namespace {
// Azahar's v1 Z3DS header precedes optional metadata and a seekable Zstandard stream.
u64 Little(const u8* field, unsigned length) {
    u64 result = 0;
    for (unsigned i = 0; i < length; ++i)
        result |= static_cast<u64>(field[i]) << (8 * i);
    return result;
}

class Z3dsReader final : public IOHandler {
public:
    explicit Z3dsReader(std::unique_ptr<IOHandler> input) : source(std::move(input)) {}

    bool Initialize() {
        std::array<u8, 32> header{};
        if (!decoder || !source->Seek(0, SEEK_SET) ||
            source->Read(header.data(), 1, header.size()) != header.size() ||
            std::memcmp(header.data(), "Z3DS", 4) != 0 || header[8] != 1)
            return false;
        const u64 header_size = Little(&header[10], 2);
        start = header_size + Little(&header[12], 4);
        const u64 compressed = Little(&header[16], 8);
        length = Little(&header[24], 8);
        physical = source->GetSize();
        if (header_size < header.size() || start > physical || compressed != physical - start ||
            compressed < 17 || !length || length > (8ULL << 30) ||
            physical > static_cast<u64>(std::numeric_limits<s64>::max()))
            return false;

        // Bound the allocation before the upstream reader loads its seek table.
        std::array<u8, 9> footer{};
        if (!source->Seek(-9, SEEK_END) || source->Read(footer.data(), 1, 9) != 9)
            return false;
        const u64 frames = Little(footer.data(), 4);
        const u64 table_bytes = 17 + frames * ((footer[4] & 0x80) ? 12 : 8);
        if (!frames || frames > (1U << 22) || table_bytes >= compressed)
            return false;
        ZSTD_seekable_customFile stream{this, ReadSource, SeekSource};
        if (ZSTD_isError(ZSTD_seekable_initAdvanced(decoder.get(), stream)))
            return false;
        const auto last = static_cast<unsigned>(frames - 1);
        return ZSTD_seekable_getFrameCompressedOffset(decoder.get(), last) +
                   ZSTD_seekable_getFrameCompressedSize(decoder.get(), last) == compressed - table_bytes &&
               ZSTD_seekable_getFrameDecompressedOffset(decoder.get(), last) +
                   ZSTD_seekable_getFrameDecompressedSize(decoder.get(), last) == length;
    }

    std::size_t Read(void* buffer, std::size_t size, std::size_t count) override {
        if (!size || !count || count > std::numeric_limits<std::size_t>::max() / size)
            return 0;
        const auto bytes = static_cast<std::size_t>(std::min<u64>(length - position, size * count));
        if (!bytes)
            return 0;
        const auto read = ZSTD_seekable_decompress(decoder.get(), buffer, bytes, position);
        if (ZSTD_isError(read))
            return 0;
        position += read;
        return read / size;
    }

    bool Seek(s64 offset, int whence) override {
        s64 base = 0;
        switch (whence) {
        case SEEK_SET: break;
        case SEEK_CUR: base = static_cast<s64>(position); break;
        case SEEK_END: base = static_cast<s64>(length); break;
        default: return false;
        }
        if (offset < -base || offset > static_cast<s64>(length) - base)
            return false;
        position = static_cast<u64>(base + offset);
        return true;
    }
    u64 Tell() override { return position; }
    u64 GetSize() override { return length; }
    std::size_t Write(const void*, std::size_t, std::size_t) override { return 0; }
    bool Resize(u64) override { return false; }
    bool Flush() override { return true; }

private:
    static int ReadSource(void* opaque, void* buffer, std::size_t size) {
        auto& reader = *static_cast<Z3dsReader*>(opaque);
        return reader.source->Read(buffer, 1, size) == size ? 0 : -1;
    }
    static int SeekSource(void* opaque, long long offset, int origin) {
        auto& reader = *static_cast<Z3dsReader*>(opaque);
        const auto start = static_cast<s64>(reader.start);
        const auto end = static_cast<s64>(reader.physical);
        if (origin != SEEK_SET && origin != SEEK_END)
            return -1;
        const auto base = origin == SEEK_SET ? start : end;
        if (offset < start - base || offset > end - base)
            return -1;
        return reader.source->Seek(base + offset, SEEK_SET) ? 0 : -1;
    }

    std::unique_ptr<IOHandler> source;
    std::unique_ptr<ZSTD_seekable, decltype(&ZSTD_seekable_free)> decoder{
        ZSTD_seekable_create(), ZSTD_seekable_free};
    u64 physical = 0;
    u64 start = 0;
    u64 length = 0;
    u64 position = 0;
};
} // namespace

std::unique_ptr<IOHandler> OpenZ3dsRom(std::unique_ptr<IOHandler> source) {
    auto reader = std::make_unique<Z3dsReader>(std::move(source));
    if (!reader->Initialize())
        return {};
    return reader;
}
} // namespace FileUtil
