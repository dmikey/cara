// SPDX-License-Identifier: GPL-2.0-or-later
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include "common/file_util.h"
#include "core/file_sys/archive_sdmc.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/savedata_archive.h"

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        const auto prefix = "cara-guest-fs-host-failure-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        for (int attempt = 0; attempt < 100; ++attempt) {
            root = std::filesystem::temp_directory_path() / (prefix + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(root)) return;
        }
        throw std::runtime_error("cannot allocate fresh fixture folder");
    }
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};
class ResizeDenied final : public FileUtil::IOHandler {
    std::size_t Read(void*, std::size_t, std::size_t) override { return 0; }
    std::size_t Write(const void*, std::size_t, std::size_t) override { return 0; }
    bool Seek(s64, int) override { return false; }
    u64 Tell() override { return 0; }
    u64 GetSize() override { return 0; }
    bool Resize(u64) override { errno = EACCES; return false; }
    bool Flush() override { return false; }
};
class FaultFactory final : public FileUtil::IOFactory {
public:
    explicit FaultFactory(int error, bool fail_resize = false)
        : error(error), fail_resize(fail_resize) {}
    std::unique_ptr<FileUtil::IOHandler> Open(const std::string&, const char mode[]) override {
        Check(std::string(mode) == "wb" || std::string(mode) == "r+b", "unexpected mode");
        if (fail_resize) return std::make_unique<ResizeDenied>();
        errno = error;
        return nullptr;
    }
private:
    int error;
    bool fail_resize;
};
struct ResetFactory {
    ~ResetFactory() { FileUtil::RegisterIOFactory(nullptr); }
};
void CheckClosed(FileUtil::IOFile& file) {
    Check(!file.IsOpen() && !file.IsGood(), "closed IOFile reported open");
    Check(!file.Seek(0, SEEK_SET) && !file.Resize(64) && !file.Flush(), "closed operation succeeded");
    Check(file.Tell() == 0 && file.GetSize() == 0, "closed query returned nonzero");
}
void CheckDenied(ResultCode result) {
    Check(result.IsError() && result.description.ExtractValue(result.raw) ==
          static_cast<u32>(ErrorDescription::NotAuthorized), "guest did not receive authorization error");
}
void CheckArchive(FileSys::ArchiveBackend& archive, const std::filesystem::path& root) {
    const FileSys::Path empty("/empty.bin"), sized("/sized.bin");
    {
        ResetFactory reset;
        FileUtil::RegisterIOFactory(std::make_unique<FaultFactory>(EACCES));
        CheckDenied(archive.CreateFile(empty, 0));
        CheckDenied(archive.CreateFile(sized, 64));
        FileSys::Mode create_mode{};
        create_mode.hex = 6; // Create + write.
        CheckDenied(archive.OpenFile(FileSys::Path("/open-create.bin"), create_mode).Code());
        FileUtil::IOFile denied((root / "denied.bin").string(), "wb");
        CheckClosed(denied);
        Check(denied.GetOpenError() == EACCES, "lost host open errno");
        FileUtil::IOFile moved(std::move(denied));
        CheckClosed(moved);
        Check(moved.GetOpenError() == EACCES, "lost moved host error");
        FileUtil::RegisterIOFactory(std::make_unique<FaultFactory>(ENOSPC));
        Check(archive.CreateFile(empty, 0) == FileSys::ERROR_INSUFFICIENT_SPACE, "space error lost");
        FileUtil::RegisterIOFactory(std::make_unique<FaultFactory>(0));
        Check(archive.CreateFile(sized, 64).IsError(), "unknown open error reported success");
        FileUtil::RegisterIOFactory(std::make_unique<FaultFactory>(0, true));
        CheckDenied(archive.CreateFile(sized, 64));
    }
    Check(!std::filesystem::exists(root / "empty.bin") && !std::filesystem::exists(root / "sized.bin"),
          "denied create unexpectedly produced files");
    Check(archive.CreateFile(empty, 0) == RESULT_SUCCESS, "valid empty create failed");
    Check(archive.CreateFile(sized, 64) == RESULT_SUCCESS, "valid sized create failed");
    Check(std::filesystem::file_size(root / "empty.bin") == 0 &&
          std::filesystem::file_size(root / "sized.bin") == 64, "valid file size changed");
    Check(archive.CreateFile(empty, 0).IsError(), "duplicate create accepted");
}
}
int main() {
    try {
        Fixture fixture;
        FileUtil::IOFile file;
        CheckClosed(file);
        Check(!file.Open((fixture.root / "missing" / "file.bin").string(), "wb"), "missing parent opened");
        CheckClosed(file);
        Check(file.GetOpenError() == ENOENT || file.GetOpenError() == ENOTDIR, "lost missing parent error");
        Check(file.Open((fixture.root / "valid.bin").string(), "w+b"), "native open failed");
        Check(file.GetOpenError() == 0 && file.Resize(64) && file.GetSize() == 64 && file.Flush(), "valid IO changed");
        file.Close();
        CheckClosed(file);
        const auto savedata = fixture.root / "save", sdmc = fixture.root / "sdmc";
        std::filesystem::create_directory(savedata);
        std::filesystem::create_directory(sdmc);
        FileSys::SaveDataArchive save_archive(savedata.string() + "/");
        FileSys::SDMCArchive sd_archive(sdmc.string() + "/", nullptr);
        CheckArchive(save_archive, savedata);
        CheckArchive(sd_archive, sdmc);
        std::cout << "PASS: closed and failed IOFile; both archive CreateFile paths preserve authorization/space/unknown failures; valid empty and sized creates remain exact\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
