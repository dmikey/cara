// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

namespace Cara {
// Skin assets stay in memory: archive paths are never extracted to the filesystem.
inline std::map<std::string, std::vector<std::uint8_t>> ReadSkinArchive(
    const std::vector<std::uint8_t>& data) {
    const auto fail = [] { throw std::runtime_error("Invalid or oversized skin archive"); };
    if (data.size() < 22 || data.size() > 32 * 1024 * 1024) fail();
    const auto read = [&](size_t p, unsigned n) -> std::uint32_t {
        if (p > data.size() || n > data.size() - p) { fail(); return 0; }
        std::uint32_t value = 0;
        for (unsigned i = 0; i < n; ++i) value |= std::uint32_t(data[p+i]) << (i*8);
        return value;
    };
    size_t end = data.size()-22;
    const size_t lower = data.size() > 65557 ? data.size()-65557 : 0;
    while (read(end,4) != 0x06054b50 || end+22+read(end+20,2) != data.size()) {
        if (end == lower) fail();
        --end;
    }
    const unsigned count = read(end+10,2);
    size_t pos = read(end+16,4), total = 0;
    const size_t directory_start=pos;
    const size_t directory_end = pos + read(end+12,4);
    if (read(end+4,2) || read(end+6,2) || read(end+8,2) != count ||
        !count || count > 64 || directory_end > end) fail();
    std::map<std::string,std::vector<std::uint8_t>> files;
    for (unsigned entry = 0; entry < count; ++entry) {
        if (pos+46 > directory_end || read(pos,4) != 0x02014b50) fail();
        const auto flags=read(pos+8,2), method=read(pos+10,2), crc=read(pos+16,4);
        const size_t packed=read(pos+20,4), size=read(pos+24,4), name_size=read(pos+28,2);
        const size_t next=pos+46+name_size+read(pos+30,2)+read(pos+32,2);
        if ((flags & 1) || (method != 0 && method != 8) || size > 16*1024*1024 ||
            packed > 32*1024*1024 || next > directory_end || !name_size) fail();
        std::string name(reinterpret_cast<const char*>(data.data()+pos+46),name_size);
        if (name.front()=='/' || name.find('\\')!=std::string::npos ||
            name.find('\0')!=std::string::npos || name.find("..")!=std::string::npos) fail();
        const size_t local=read(pos+42,4);
        if (local+30 > directory_start || read(local,4)!=0x04034b50 || read(local+8,2)!=method || read(local+6,2)!=flags) fail();
        const size_t body=local+30+read(local+26,2)+read(local+28,2);
        if (body > directory_start || packed > directory_start-body || total+size > 64*1024*1024 || read(local+26,2)!=name_size) fail();
        if (std::string(reinterpret_cast<const char*>(data.data()+local+30),name_size)!=name) fail();
        if (!(flags&8) && (read(local+14,4)!=crc || read(local+18,4)!=packed || read(local+22,4)!=size)) fail();
        total += size;
        if (name.back() != '/') {
            std::vector<std::uint8_t> output(size);
            if (method == 0) {
                if (packed != size) fail();
                std::copy_n(data.data()+body,size,output.data());
            } else {
                z_stream stream{};
                stream.next_in=const_cast<Bytef*>(data.data()+body); stream.avail_in=static_cast<uInt>(packed);
                // One byte of capacity also handles valid empty deflate streams.
                std::uint8_t empty=0;
                stream.next_out=size?output.data():&empty; stream.avail_out=static_cast<uInt>(size?size:1);
                if (inflateInit2(&stream,-MAX_WBITS)!=Z_OK) fail();
                const int result=inflate(&stream,Z_FINISH);
                const bool valid=result==Z_STREAM_END && stream.total_out==size && stream.total_in==packed;
                inflateEnd(&stream);
                if (!valid) fail();
            }
            if (crc32(0,output.data(),static_cast<uInt>(size)) != crc || !files.emplace(name,std::move(output)).second) fail();
        }
        pos=next;
    }
    if (pos != directory_end || !files.count("info.json")) fail();
    return files;
}
} // namespace Cara
