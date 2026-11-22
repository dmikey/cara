// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/sevenzip_lzma2.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <optional>
#include <vector>
#ifdef _WIN32
#include <random>
#include <windows.h>
#include "common/string_util.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <fmt/format.h>
#include <lzma.h>
#include <zlib.h>
#include "common/logging/log.h"

namespace FileUtil {
namespace {
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
constexpr u64 BlockSize = 256 * 1024;
constexpr std::size_t BlockCount = 192; // 48 MiB shared across both streams.
constexpr u64 NearSeek = 32ULL << 20;
constexpr u64 FarSeekTail = 4ULL << 20;
constexpr std::size_t MaxPoints = 8192;
struct Point {
    u64 unpacked, packed;
};
static_assert(sizeof(Point) == 16);
struct IndexHeader {
    u64 magic, count, checksum;
};
constexpr u64 IndexMagic = 0x32495a3741524143ULL;
using Progress = SevenZipLoadScope::Progress;

bool ReadAt(IOHandler& file, u64 at, void* data, std::size_t n) {
    return file.Seek(static_cast<s64>(at), SEEK_SET) && file.Read(data, 1, n) == n;
}

std::string SourceStamp(const std::string& path, IOHandler& file) {
#ifdef _WIN32
    const auto source = CreateFileW(Common::UTF8ToUTF16W(path).c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (source == INVALID_HANDLE_VALUE)
        return {};
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO times{};
    const bool got_info = GetFileInformationByHandle(source, &info) &&
                          GetFileInformationByHandleEx(source, FileBasicInfo, &times, sizeof(times));
    CloseHandle(source);
    std::array<u8, 32> header;
    const u64 physical = file.GetSize();
    const u64 observed = (u64{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
    if (!got_info || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || observed != physical ||
        !ReadAt(file, 0, header.data(), header.size()) || !file.Seek(0, SEEK_SET))
        return {};
    return fmt::format("win-{:x}-{:x}-{:x}-{:x}-{:x}-{:x}-{:x}", info.dwVolumeSerialNumber,
                       info.nFileIndexHigh, info.nFileIndexLow, physical, times.LastWriteTime.QuadPart,
                       times.ChangeTime.QuadPart, crc32(0, header.data(), header.size()));
#else
    struct stat st{};
    std::array<u8, 32> header;
    const u64 physical = file.GetSize();
    if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        static_cast<u64>(st.st_size) != physical ||
        !ReadAt(file, 0, header.data(), header.size()) || !file.Seek(0, SEEK_SET))
        return {};
#ifdef __APPLE__
    const auto modified = st.st_mtimespec, changed = st.st_ctimespec;
#else
    const auto modified = st.st_mtim, changed = st.st_ctim;
#endif
    return fmt::format("{:x}-{:x}-{:x}-{:x}-{:x}-{:x}-{:x}-{:x}", st.st_dev, st.st_ino, physical,
                       modified.tv_sec, modified.tv_nsec, changed.tv_sec, changed.tv_nsec,
                       crc32(0, header.data(), header.size()));
#endif
}

bool LoadIndex(const fs::path& path, const cara_lzma2_layout& layout, std::vector<Point>& points) {
    std::ifstream f(path, std::ios::binary);
    IndexHeader h{};
    if (!f.read(reinterpret_cast<char*>(&h), sizeof(h)) || h.magic != IndexMagic || !h.count ||
        h.count > MaxPoints)
        return false;
    std::vector<Point> stored(h.count);
    const auto bytes = stored.size() * sizeof(Point);
    if (!f.read(reinterpret_cast<char*>(stored.data()), bytes) || f.peek() != EOF ||
        crc32(0, reinterpret_cast<const Bytef*>(stored.data()), bytes) != h.checksum ||
        stored.front().unpacked || stored.front().packed ||
        stored.back().unpacked >= layout.folder_size || stored.back().packed >= layout.pack_size)
        return false;
    for (std::size_t i = 1; i < stored.size(); ++i)
        if (stored[i].unpacked <= stored[i - 1].unpacked ||
            stored[i].packed <= stored[i - 1].packed)
            return false;
    points = std::move(stored);
    return true;
}

void StoreIndex(const fs::path& path, const std::vector<Point>& points) {
    if (path.empty())
        return;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
        return;
    const auto bytes = points.size() * sizeof(Point);
    const IndexHeader h{IndexMagic, points.size(),
                        crc32(0, reinterpret_cast<const Bytef*>(points.data()), bytes)};
#ifdef _WIN32
    auto temp = path;
    temp += L".partial-" + std::to_wstring(std::random_device{}());
    const auto handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return; // A concurrent writer will produce a valid index.
    DWORD written = 0;
    const bool ok = WriteFile(handle, &h, sizeof(h), &written, nullptr) && written == sizeof(h) &&
                    WriteFile(handle, points.data(), static_cast<DWORD>(bytes), &written, nullptr) &&
                    written == bytes;
    if (!CloseHandle(handle) || !ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(temp.c_str());
#else
    auto temp = path.string() + ".partial-XXXXXX";
    int fd = mkstemp(temp.data());
    if (fd < 0)
        return;
    const bool ok = write(fd, &h, sizeof(h)) == sizeof(h) &&
                    write(fd, points.data(), bytes) == static_cast<ssize_t>(bytes);
    if (close(fd) != 0 || !ok || rename(temp.c_str(), path.c_str()) != 0)
        unlink(temp.c_str());
#endif
    // An index contains offsets only. Bound the directory even across many replaced archives.
    std::vector<std::pair<fs::file_time_type, fs::path>> candidates;
    for (fs::directory_iterator it(path.parent_path(), ec), end; !ec && it != end;
         it.increment(ec)) {
        auto name = it->path().filename().string();
        if (!name.starts_with("v1-") || it->path() == path)
            continue;
        // A process may terminate between mkstemp and rename. Give concurrent
        // writers ample time to finish before removing an abandoned index.
        if (name.find(".idx.partial-") != std::string::npos) {
            std::error_code ignored;
            auto time = it->last_write_time(ignored);
            if (!ignored && fs::file_time_type::clock::now() - time > std::chrono::hours(24))
                fs::remove(it->path(), ignored);
            continue;
        }
        if (!name.ends_with(".idx"))
            continue;
        auto modified = it->last_write_time(ec);
        if (!ec)
            candidates.emplace_back(modified, it->path());
    }
    if (candidates.size() > 31) {
        std::sort(candidates.begin(), candidates.end());
        for (std::size_t i = 0; i < candidates.size() - 31; ++i)
            fs::remove(candidates[i].second, ec);
    }
}

bool Scan(IOHandler& file, const cara_lzma2_layout& l, std::vector<Point>& points,
          const Progress& progress) {
    u64 p = 0, u = 0, last_report = 0;
    unsigned resets = 0, stride = 1;
    std::array<u8, 6> header{};
    for (unsigned packets = 0; packets < (1U << 22) && p < l.pack_size; ++packets) {
        const auto n = std::min<u64>(header.size(), l.pack_size - p);
        if (!ReadAt(file, l.pack_start + p, header.data(), n))
            return false;
        const auto ctrl = header[0];
        if (!ctrl)
            return p + 1 == l.pack_size && u == l.folder_size && !points.empty();
        if ((ctrl > 2 && ctrl < 128) || n < 3)
            return false;
        const bool reset = ctrl == 1 || ctrl >= 224;
        if (!p && !reset)
            return false;
        if (reset && resets++ % stride == 0) {
            if (points.size() == MaxPoints) {
                for (std::size_t i = 0; i < MaxPoints / 2; ++i)
                    points[i] = points[2 * i];
                points.resize(MaxPoints / 2);
                stride *= 2;
            }
            points.push_back({u, p});
        }
        const u64 length =
            1 + ((header[1] << 8) | header[2]) + (ctrl >= 128 ? (u64{ctrl & 31U} << 16) : 0);
        u64 packed = length, prefix = 3;
        if (ctrl >= 128) {
            prefix = ctrl >= 192 ? 6 : 5;
            if (n < prefix)
                return false;
            packed = 1 + ((header[3] << 8) | header[4]);
        }
        if (prefix + packed > l.pack_size - p || length > l.folder_size - u)
            return false;
        p += prefix + packed;
        u += length;
        if (progress && p - last_report >= (16U << 20)) {
            progress(p, l.pack_size);
            last_report = p;
        }
    }
    return false;
}

class IndexedLzma2 final : public SevenZipCartridge {
public:
    IndexedLzma2(std::unique_ptr<IOHandler> input, const cara_lzma2_layout& l,
                 std::vector<Point> idx)
        : source(std::move(input)), layout(l), points(std::move(idx)),
          checksums((l.member_size + BlockSize - 1) / BlockSize) {
        size = l.member_size;
        points.push_back({layout.folder_size, layout.pack_size});
    }
    ~IndexedLzma2() {
        LOG_INFO(Common_Filesystem,
                 "7z demand reads: {} restarts, {} decoded bytes for {} requested, {} cached "
                 "blocks served",
                 restarts, decoded, requested, hits);
    }

    std::size_t ReadAt(u64 at, void* dst, std::size_t count) override {
        std::lock_guard lock(mutex);
        if (failed || at > size)
            return 0;
        count = static_cast<std::size_t>(std::min<u64>(size - at, count));
        requested += count;
        std::size_t done = 0;
        while (done < count) {
            const u64 index = at / BlockSize;
            auto it = std::find_if(cache.begin(), cache.end(),
                                   [&](const Block& b) { return b.index == index; });
            if (it == cache.end()) {
                const auto before = decoded;
                if (!Load(index)) {
                    failed = true;
                    return done;
                }
                if (std::getenv("CARA_TRACE_7Z_READS") && decoded - before > 1048576)
                    LOG_INFO(Common_Filesystem,
                             "7z slow seek: position={} block={} decoded={} requested={}", at,
                             index, decoded - before, count);
            } else {
                ++hits;
                cache.splice(cache.begin(), cache, it);
            }
            const auto& bytes = cache.front().data;
            const std::size_t off = at % BlockSize;
            const auto n = std::min(bytes.size() - off, count - done);
            std::memcpy(static_cast<u8*>(dst) + done, bytes.data() + off, n);
            done += n;
            at += n;
        }
        return done;
    }

private:
    struct Block {
        u64 index;
        std::vector<u8> data;
    };
    struct Cursor {
        lzma_stream decoder = LZMA_STREAM_INIT;
        std::array<u8, 65536> input{};
        std::size_t segment = 0;
        u64 packed = 0, position = 0;
        bool ready = false, terminated = false;
        ~Cursor() {
            lzma_end(&decoder);
        }
    };
    bool Start(std::size_t i) {
        lzma_end(&current->decoder);
        current->decoder = LZMA_STREAM_INIT;
        const auto prop = layout.properties;
        lzma_options_lzma options{};
        options.dict_size = prop == 40 ? UINT32_MAX : (2U | (prop & 1U)) << (prop / 2 + 11);
        const lzma_filter filters[]{{LZMA_FILTER_LZMA2, &options}, {LZMA_VLI_UNKNOWN, nullptr}};
        if (lzma_raw_decoder(&current->decoder, filters) != LZMA_OK)
            return false;
        current->segment = i;
        current->packed = points[i].packed;
        current->position = points[i].unpacked;
        current->terminated = false;
        current->ready = true;
        ++restarts;
        return true;
    }
    bool Fill() {
        if (current->decoder.avail_in)
            return true;
        if (current->packed < points[current->segment + 1].packed) {
            auto n = std::min<u64>(current->input.size(),
                                   points[current->segment + 1].packed - current->packed);
            if (!FileUtil::ReadAt(*source, layout.pack_start + current->packed,
                                  current->input.data(), n))
                return false;
            current->decoder.next_in = current->input.data();
            current->decoder.avail_in = n;
            current->packed += n;
            return true;
        }
        // Each dictionary reset is independently decodable. Terminate the raw
        // stream at this boundary; the last span already includes its real terminator.
        if (current->segment + 2 == points.size() || current->terminated)
            return false;
        current->input[0] = 0;
        current->decoder.next_in = current->input.data();
        current->decoder.avail_in = 1;
        current->terminated = true;
        return true;
    }
    bool Decode(void* dst, std::size_t count) {
        auto* output = static_cast<u8*>(dst);
        while (count) {
            if (current->position == points[current->segment + 1].unpacked &&
                !Start(current->segment + 1))
                return false;
            const auto n =
                std::min<u64>(count, points[current->segment + 1].unpacked - current->position);
            current->decoder.next_out = output;
            current->decoder.avail_out = n;
            lzma_ret ret = LZMA_OK;
            while (current->decoder.avail_out) {
                if (!Fill())
                    return false;
                const auto before = current->decoder.avail_in;
                const auto left = current->decoder.avail_out;
                ret = lzma_code(&current->decoder, LZMA_RUN);
                if ((ret != LZMA_OK && ret != LZMA_STREAM_END) ||
                    (left == current->decoder.avail_out && before == current->decoder.avail_in) ||
                    (ret == LZMA_STREAM_END && current->decoder.avail_out))
                    return false;
            }
            current->position += n;
            decoded += n;
            output += n;
            count -= n;
            if (current->position == points[current->segment + 1].unpacked) {
                u8 extra;
                current->decoder.next_out = &extra;
                current->decoder.avail_out = 1;
                while (ret != LZMA_STREAM_END) {
                    if (!Fill())
                        return false;
                    const auto before = current->decoder.avail_in;
                    ret = lzma_code(&current->decoder, LZMA_RUN);
                    if (!current->decoder.avail_out ||
                        (ret != LZMA_STREAM_END &&
                         (ret != LZMA_OK || before == current->decoder.avail_in)))
                        return false;
                }
                if (current->decoder.avail_in ||
                    current->packed != points[current->segment + 1].packed)
                    return false;
            } else if (ret == LZMA_STREAM_END)
                return false;
        }
        return true;
    }
    bool DecodeBlock(u64 index) {
        if (cache.size() == BlockCount)
            cache.pop_back();
        auto& block = cache.emplace_front(Block{index, {}});
        block.data.resize(std::min<u64>(BlockSize, size - index * BlockSize));
        if (!Decode(block.data.data(), block.data.size()))
            return false;
        if (layout.has_crc && !checksums[index]) {
            checksums[index] = crc32(0, block.data.data(), block.data.size());
            if (++verified_blocks == checksums.size()) {
                uLong crc = 0;
                for (std::size_t j = 0; j < checksums.size(); ++j)
                    crc = crc32_combine(crc, *checksums[j],
                                        std::min<u64>(BlockSize, size - j * BlockSize));
                if (crc != layout.member_crc)
                    return false;
            }
        }
        return true;
    }
    bool Load(u64 index) {
        const auto at = layout.member_offset + index * BlockSize;
        auto next = std::upper_bound(points.begin(), points.end(), at,
                                     [](u64 value, const Point& p) { return value < p.unpacked; });
        const auto start = static_cast<std::size_t>(next - points.begin() - 1);
        const auto usable = [&](const Cursor& c) {
            return c.ready && c.position >= points[start].unpacked && c.position <= at;
        };
        if (usable(*parked) && (!usable(*current) || parked->position > current->position))
            std::swap(current, parked);
        if (!usable(*current)) {
            // Leave a decoder at the previous forward read when switching solid
            // regions. At most 128 MiB of LZMA dictionaries are live.
            if (layout.properties <= 28 && current->ready && current->segment != start)
                std::swap(current, parked);
            if (!Start(start))
                return false;
        }
        // Nearby backward reads frequently target the gap we just decoded. Keep
        // those pages; on a long walk retain only its tail so other hot ranges survive.
        const auto distance = at - current->position;
        auto retain = std::max(layout.member_offset,
                               distance > NearSeek ? at - FarSeekTail : current->position);
        retain = std::min(at, layout.member_offset +
                                  ((retain - layout.member_offset + BlockSize - 1) / BlockSize) *
                                      BlockSize);
        while (current->position < retain)
            if (!Decode(scratch.data(), std::min<u64>(retain - current->position, scratch.size())))
                return false;
        while (current->position < at) {
            const auto intervening = (current->position - layout.member_offset) / BlockSize;
            const bool present = std::any_of(
                cache.begin(), cache.end(), [&](const Block& b) { return b.index == intervening; });
            if (present) {
                for (u64 n = 0; n < BlockSize; n += scratch.size())
                    if (!Decode(scratch.data(), scratch.size()))
                        return false;
            } else if (!DecodeBlock(intervening))
                return false;
        }
        return DecodeBlock(index);
    }
    std::unique_ptr<IOHandler> source;
    cara_lzma2_layout layout;
    std::vector<Point> points;
    std::vector<std::optional<u32>> checksums;
    std::size_t verified_blocks = 0;
    std::list<Block> cache;
    std::mutex mutex;
    std::unique_ptr<Cursor> current = std::make_unique<Cursor>();
    std::unique_ptr<Cursor> parked = std::make_unique<Cursor>();
    std::array<u8, 65536> scratch{};
    u64 restarts = 0, decoded = 0, requested = 0, hits = 0;
    bool failed = false;
};
} // namespace

std::string SevenZipSourceStamp(const std::string& path, IOHandler& source) {
    return SourceStamp(path, source);
}

std::shared_ptr<SevenZipCartridge> OpenIndexedLzma2(std::unique_ptr<IOHandler> source,
                                                    const cara_lzma2_layout& l,
                                                    const std::string& path,
                                                    const std::string& directory,
                                                    const Progress& progress) {
    const u64 physical = source->GetSize();
    if (l.pack_start > physical || l.pack_size > physical - l.pack_start || !l.pack_size ||
        !l.member_size || l.member_size > (8ULL << 30) || l.member_offset > l.folder_size ||
        l.member_size > l.folder_size - l.member_offset)
        return {};
    const auto start = Clock::now();
    fs::path index;
    const auto stamp = directory.empty() ? std::string{} : SourceStamp(path, *source);
    if (!stamp.empty())
        index =
            fs::path(directory) / fmt::format("v1-{}-{:x}-{:x}-{:x}-{:x}.idx", stamp, l.pack_start,
                                              l.pack_size, l.folder_size, l.member_crc);
    std::vector<Point> points;
    const bool cached = !index.empty() && LoadIndex(index, l, points);
    if (!cached) {
        if (progress)
            progress(0, l.pack_size);
        if (!Scan(*source, l, points, progress))
            return {};
        if (!index.empty() && SourceStamp(path, *source) == stamp)
            StoreIndex(index, points);
    }
    if (progress)
        progress(l.pack_size, l.pack_size);
    LOG_INFO(Common_Filesystem,
             "7z LZMA2 restart index {}: {} points, {} bytes, {:.3f}s; cartridge {} bytes",
             cached ? "hit" : "built", points.size(),
             sizeof(IndexHeader) + points.size() * sizeof(Point),
             std::chrono::duration<double>(Clock::now() - start).count(), l.member_size);
    return std::make_shared<IndexedLzma2>(std::move(source), l, std::move(points));
}
} // namespace FileUtil
