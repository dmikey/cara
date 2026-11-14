// SPDX-License-Identifier: GPL-2.0-or-later
// ROM-free regression for the actual archive/factory and IOFile serializers.
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <boost/serialization/unique_ptr.hpp>
#include "common/archives.h"
#include "common/file_util.h"
#include "core/file_sys/archive_extsavedata.h"
#include "core/file_sys/archive_savedata.h"
#include "core/file_sys/archive_sdmc.h"
#include "core/file_sys/archive_sdmcwriteonly.h"
#include "core/file_sys/archive_systemsavedata.h"
#include "core/file_sys/file_backend.h"
#include "core/file_sys/savedata_archive.h"

namespace {
constexpr u64 title = 0x0004000000CAFE00;
constexpr std::array<const char*, 6> names{"savedata", "sdmc", "sdmc-write-only", "system", "ext", "shared-ext"};
void Check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
FileSys::Path ArchivePath(std::size_t index) {
    if (index == 3) return FileSys::ConstructSystemSaveDataBinaryPath(0, 0xCAFE);
    if (index >= 4) return FileSys::ConstructExtDataBinaryPath(index == 4 ? 1 : 0, 0, 0xCAFE);
    return FileSys::Path();
}
FileSys::Path GuestPath(std::size_t index) {
    return FileSys::Path(("/" + std::string(names[index]) + ".bin").c_str());
}
void Write(FileSys::ArchiveBackend& archive, std::size_t index, bool create) {
    const auto path = GuestPath(index);
    if (create) Check(archive.CreateFile(path, 8).IsSuccess(), "create " + std::string(names[index]));
    FileSys::Mode mode{};
    mode.hex = 2; // Write-only archives deliberately reject the read bit.
    auto opened = archive.OpenFile(path, mode);
    Check(opened.Succeeded(), "open " + std::string(names[index]) + ": " + archive.GetName());
    auto file = std::move(opened).Unwrap();
    const std::array<u8, 8> original{'S','E','N','T','I','N','E','L'};
    const std::array<u8, 8> restored{'R','E','L','O','C','A','T','E'};
    const auto& bytes = create ? original : restored;
    auto result = file->Write(0, bytes.size(), true, bytes.data());
    Check(result.Succeeded() && result.Unwrap() == bytes.size(), "write failed");
    file->Close();
}
std::string Slash(const char* path) {
    return std::filesystem::absolute(path).string() + "/";
}
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "Usage: cara-save-archive-relocation-self-test save|load USER_ROOT STATE_FILE EXTERNAL_ROOT\n";
        return 2;
    }
    try {
        const std::string mode = argv[1], root = Slash(argv[2]), external = Slash(argv[4]);
        Check(mode == "save" || mode == "load", "unknown mode");
        FileUtil::SetUserPath(root); // One root per process, like a fresh app launch.
        FileUtil::SetCurrentRomPath("/authored/nonexistent-fixture.elf");
        Check(FileUtil::SerializePath(external, true) == external, "external path changed on save");
        Check(FileUtil::SerializePath(external, false) == external, "external path changed on load");
        Check(FileUtil::SerializePath(root + "sdmc/", true) == "%CARA_USER_DIR%sdmc/",
              "user path was not tokenized");
        std::vector<std::unique_ptr<FileSys::ArchiveFactory>> factories;
        std::vector<std::unique_ptr<FileSys::ArchiveBackend>> active;
        std::unique_ptr<FileSys::ArchiveBackend> outside;
        std::unique_ptr<FileSys::ArchiveFactory> outside_factory;
        FileUtil::IOFile open_file;
        const auto io_path = root + "open-file.bin";
        if (mode == "save") {
            auto source = std::make_shared<FileSys::ArchiveSource_SDSaveData>(root + "sdmc/");
            factories.emplace_back(std::make_unique<FileSys::ArchiveFactory_SaveData>(source));
            factories.emplace_back(std::make_unique<FileSys::ArchiveFactory_SDMC>(root + "sdmc/"));
            factories.emplace_back(std::make_unique<FileSys::ArchiveFactory_SDMCWriteOnly>(root + "sdmc-write-only/"));
            factories.emplace_back(std::make_unique<FileSys::ArchiveFactory_SystemSaveData>(root + "nand/"));
            factories.emplace_back(std::make_unique<FileSys::ArchiveFactory_ExtSaveData>(root + "sdmc/", false));
            factories.emplace_back(std::make_unique<FileSys::ArchiveFactory_ExtSaveData>(root + "nand/", true));
            FileSys::ArchiveFormatInfo format{};
            for (std::size_t i = 0; i < factories.size(); ++i) {
                if (i == 1 || i == 2) {
                    std::filesystem::create_directories(root + (i == 1 ? "sdmc/" : "sdmc-write-only/"));
                } else {
                    Check(factories[i]->Format(ArchivePath(i), format, title).IsSuccess(), "format failed");
                }
                auto archive = factories[i]->Open(ArchivePath(i), title);
                Check(archive.Succeeded(), "factory open failed");
                active.emplace_back(std::move(archive).Unwrap());
                Write(*active.back(), i, true);
            }
            std::filesystem::create_directories(external);
            outside = std::make_unique<FileSys::SaveDataArchive>(external);
            outside_factory = std::make_unique<FileSys::ArchiveFactory_SDMC>(external);
            std::ofstream(io_path, std::ios::binary) << "IO-SENTINEL";
            Check(open_file.Open(io_path, "r+b"), "IOFile fixture open");
            std::ofstream output(argv[3], std::ios::binary);
            oarchive archive(output);
            archive & factories & active & outside & outside_factory & open_file;
        } else {
            {
                std::ifstream input(argv[3], std::ios::binary);
                iarchive archive(input);
                archive & factories & active & outside & outside_factory & open_file;
            }
            Check(factories.size() == names.size() && active.size() == names.size(), "fixture count");
            Check(open_file.Seek(0, SEEK_SET) && open_file.WriteBytes("IO-REBASED!", 11) == 11 && open_file.Flush(),
                  "restored IOFile write failed");
            open_file.Close();
            Check(outside->GetName() == "SaveDataArchive: " + external, "external archive was rebased");
            auto external_open = outside_factory->Open(FileSys::Path(), title);
            Check(external_open.Succeeded() && (*external_open)->GetName() == "SDMCArchive: " + external,
                  "external factory was rebased");
            for (std::size_t i = 0; i < factories.size(); ++i) {
                Check(active[i]->GetName().find(root) != std::string::npos, "active archive retained old root");
                Write(*active[i], i, false);
                auto reopened = factories[i]->Open(ArchivePath(i), title);
                Check(reopened.Succeeded(), "restored factory cannot open " + std::string(names[i]));
                Check((*reopened)->GetName().find(root) != std::string::npos, "factory retained old root");
                Write(**reopened, i, false);
            }
            std::cout << "PASS: six restored archive/factory pairs write current root; external mount preserved; IOFile rebased\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
