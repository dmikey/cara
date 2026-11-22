// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "cara_7zip_layout.h"
#include "common/file_util.h"
#include "common/sevenzip_rom.h"

namespace FileUtil {
class SevenZipCartridge {
public:
    virtual ~SevenZipCartridge() = default;
    virtual std::size_t ReadAt(u64 position, void* output, std::size_t count) = 0;
    u64 size = 0;
};
std::string SevenZipSourceStamp(const std::string& path, IOHandler& source);
std::shared_ptr<SevenZipCartridge> OpenIndexedLzma2(std::unique_ptr<IOHandler> source,
                                                    const cara_lzma2_layout& layout,
                                                    const std::string& path,
                                                    const std::string& cache_directory,
                                                    const SevenZipLoadScope::Progress& progress);
} // namespace FileUtil
