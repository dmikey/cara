// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <boost/serialization/unique_ptr.hpp>
#include "common/archives.h"
#include "common/file_util.h"
#include "core/file_sys/archive_extsavedata.h"
#include "core/file_sys/file_backend.h"

static void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: cara-extdata-state-self-test EMPTY_OUTPUT_DIRECTORY\n";
        return 2;
    }
    try {
        const auto root = std::filesystem::absolute(argv[1]);
        Check(!std::filesystem::exists(root) || std::filesystem::is_empty(root),
              "Output directory must be empty");
        std::filesystem::create_directories(root);
        FileSys::ArchiveFactory_ExtSaveData factory(root.string() + "/", false);
        const auto archive_path = FileSys::ConstructExtDataBinaryPath(1, 0, 0xCA12);
        FileSys::ArchiveFormatInfo format{};
        Check(factory.Format(archive_path, format, 0).IsSuccess(), "Extdata format failed");
        auto opened_archive = factory.Open(archive_path, 0);
        Check(opened_archive.Succeeded(), "Extdata archive open failed");
        auto archive = std::move(opened_archive).Unwrap();
        const FileSys::Path file_path("/Fixed.bin");
        Check(archive->CreateFile(file_path, 8).IsSuccess(), "Fixed file creation failed");
        // Extdata intentionally grants read/write even when the caller requests read only.
        FileSys::Mode mode{};
        mode.hex = 1;
        auto opened_file = archive->OpenFile(file_path, mode);
        Check(opened_file.Succeeded(), "Fixed file open failed");
        std::unique_ptr<FileSys::FileBackend> file = std::move(opened_file).Unwrap();
        const std::array<u8, 8> initial{0, 1, 2, 3, 4, 5, 6, 7};
        auto written = file->Write(0, initial.size(), true, initial.data());
        Check(written.Succeeded() && written.Unwrap() == initial.size(), "Initial write failed");
        std::array<u8, 8> bytes{};
        auto read = file->Read(2, 3, bytes.data());
        Check(read.Succeeded() && read.Unwrap() == 3 && bytes[0] == 2 && bytes[2] == 4,
              "Initial offset read failed");
        std::ostringstream saved;
        {
            oarchive state(saved);
            state << file;
        }
        file.reset(); // The loaded backend must reopen its own file descriptor.
        {
            std::istringstream input(saved.str());
            iarchive state(input);
            state >> file;
        }
        Check(file && file->GetSize() == 8, "Restored file size changed");
        Check(!file->SetSize(3) && !file->SetSize(99), "Restored file permits resizing");
        read = file->Read(3, 2, bytes.data());
        Check(read.Succeeded() && read.Unwrap() == 2 && bytes[0] == 3 && bytes[1] == 4,
              "Restored offset read failed");
        const std::array<u8, 8> replacement{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};
        written = file->Write(6, replacement.size(), true, replacement.data());
        Check(written.Succeeded() && written.Unwrap() == 2, "Restored tail write was not clamped");
        written = file->Write(8, 2, false, replacement.data());
        Check(written.Succeeded() && written.Unwrap() == 0, "Write at EOF must write zero bytes");
        Check(file->Write(9, 1, false, replacement.data()).Failed(), "Write beyond EOF succeeded");
        written = file->Write(7, std::numeric_limits<std::size_t>::max(), false, replacement.data());
        Check(written.Succeeded() && written.Unwrap() == 1, "Overflowing write length was not clamped");
        file->Flush();
        file->Close();
        file.reset();
        auto reopened_file = archive->OpenFile(file_path, mode);
        Check(reopened_file.Succeeded(), "Archive reopen failed");
        file = std::move(reopened_file).Unwrap();
        read = file->Read(0, bytes.size(), bytes.data());
        const std::array<u8, 8> expected{0, 1, 2, 3, 4, 5, 0xA0, 0xA0};
        Check(read.Succeeded() && read.Unwrap() == bytes.size() && bytes == expected,
              "Reopened file contents mismatch");
        Check(file->GetSize() == 8 && file->GetReadDelayNs(3) > 0,
              "Reopened size or delay generator invalid");
        file.reset();
        const auto host_path = FileSys::GetExtSaveDataPath(factory.GetMountPoint(), archive_path) +
                               "user/Fixed.bin";
        FileUtil::IOFile host(host_path, "rb");
        Check(host.IsOpen() && host.GetSize() == 8 && host.ReadBytes(bytes.data(), bytes.size()) == 8 &&
                  bytes == expected,
              "Host file flush/size verification failed");
        std::cout << "Public extdata API, polymorphic state restore, reopen, offsets, fixed bounds, "
                     "overflow, resize refusal and flush passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
