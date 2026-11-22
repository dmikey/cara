// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
namespace FileUtil {
class IOHandler;

// Applies to opens on the constructing thread. An empty directory uses temporary storage.
class SevenZipLoadScope {
public:
    using Progress = std::function<void(std::uint64_t, std::uint64_t)>;
    SevenZipLoadScope(std::string cache_directory, Progress progress);
    ~SevenZipLoadScope();
    SevenZipLoadScope(const SevenZipLoadScope&) = delete;
    SevenZipLoadScope& operator=(const SevenZipLoadScope&) = delete;

private:
    friend std::unique_ptr<IOHandler> OpenSevenZipRom(const std::string&,
                                                      std::unique_ptr<IOHandler>);
    std::string directory;
    Progress report;
    const SevenZipLoadScope* previous;
};
std::unique_ptr<IOHandler> OpenSevenZipRom(const std::string& path,
                                           std::unique_ptr<IOHandler> source);
} // namespace FileUtil
