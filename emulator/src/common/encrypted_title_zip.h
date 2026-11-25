// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <archive.h>
#include <archive_entry.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace FileUtil {
// A failed cartridge ZIP may instead contain an encrypted CDN title dump.
// Inspect only bounded title metadata and member sizes. Never extract content.
inline bool HasEncryptedTitleZip(const std::string& path) {
    std::unique_ptr<archive, decltype(&archive_read_free)> reader{archive_read_new(),
                                                               archive_read_free};
    if (!reader || archive_read_support_format_zip_seekable(reader.get()) != ARCHIVE_OK ||
        archive_read_open_filename(reader.get(), path.c_str(), 64 * 1024) != ARCHIVE_OK)
        return false;
    std::map<std::string, std::uint64_t> members;
    std::vector<unsigned char> tmd;
    std::filesystem::path metadata_path;
    archive_entry* entry = nullptr;
    int status;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        if (members.size() >= 4096) return false;
        const char* raw = archive_entry_pathname(entry);
        if (!raw || archive_entry_filetype(entry) != AE_IFREG ||
            !archive_entry_size_is_set(entry) || archive_entry_size(entry) < 0)
            continue;
        const std::filesystem::path name(raw);
        if (!members.emplace(name.generic_string(), archive_entry_size(entry)).second)
            return false;
        if (name.filename() != "tmd" && name.filename() != "tmd.0")
            continue;
        const auto length = archive_entry_size(entry);
        if (!tmd.empty() || length < 0x9c4 || length > 1024 * 1024 ||
            archive_entry_is_encrypted(entry))
            return false;
        metadata_path = name;
        tmd.resize(length);
        std::size_t done = 0;
        while (done < tmd.size()) {
            const auto n = archive_read_data(reader.get(), tmd.data() + done, tmd.size() - done);
            if (n <= 0) return false;
            done += n;
        }
    }
    if (status != ARCHIVE_EOF || tmd.empty()) return false;
    const auto big = [&](std::size_t offset, unsigned bytes) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) value = (value << 8) | tmd[offset + i];
        return value;
    };
    std::size_t body;
    switch (big(0, 4)) {
    case 0x10000: case 0x10003: body = 0x240; break;
    case 0x10001: case 0x10004: body = 0x140; break;
    case 0x10002: case 0x10005: body = 0x40; break;
    default: return false;
    }
    constexpr std::size_t body_size = 0x9c4, record_size = 0x30;
    if (tmd.size() < body + body_size || big(body + 0x4c, 4) != 0x00040000)
        return false;
    const auto count = big(body + 0x9e, 2);
    const auto boot = big(body + 0xa0, 2);
    if (!count || tmd.size() < body + body_size + count * record_size)
        return false;
    bool encrypted_boot = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto record = body + body_size + i * record_size;
        char id[9];
        std::snprintf(id, sizeof(id), "%08x", static_cast<unsigned>(big(record, 4)));
        const auto found = members.find((metadata_path.parent_path() / id).generic_string());
        const auto size = big(record + 8, 8);
        if (found == members.end() || !size || found->second != size)
            return false;
        if (big(record + 4, 2) == boot)
            encrypted_boot = (big(record + 6, 2) & 1) && size % 16 == 0;
    }
    return encrypted_boot;
}
} // namespace FileUtil
