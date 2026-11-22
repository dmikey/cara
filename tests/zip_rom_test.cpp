// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <sstream>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include "common/cia_rom.h"
#include "common/file_util.h"
#include "common/logging/backend.h"

namespace {
void Require(bool value, std::string_view error) {
    if (!value)
        throw std::runtime_error(std::string(error));
}

void Compare(FileUtil::IOFile& file, std::span<const u8> expected, u64 offset, std::size_t length) {
    Require(file.Seek(offset, SEEK_SET), "Seek failed");
    std::vector<u8> read(length);
    Require(file.ReadBytes(read.data(), length) == length &&
                std::ranges::equal(read, expected.subspan(offset, length)),
            "Indexed read differed from original bytes");
    Require(file.Tell() == offset + length, "Incorrect read cursor");
}
} // namespace

int main(int argc, char** argv) {
    try {
        Log::AddBackend(std::make_unique<Log::ColorConsoleBackend>());
        Require(argc >= 4, "Pass raw fixture, valid ZIP, --bad, and invalid ZIPs");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> expected{std::istreambuf_iterator<char>(input), {}};
        Require(expected.size() > 4 * 1024 * 1024, "Raw fixture missing");
        bool invalid = false;
        unsigned archives = 0;
        for (int i = 2; i < argc; ++i) {
            const std::string_view path = argv[i];
            if (path == "--invalid-cia") {
                Require(++i < argc && !FileUtil::GetCiaTitleId(argv[i]), "Malformed CIA classified");
                continue;
            }
            if (path == "--update-cia") {
                Require(++i < argc && FileUtil::GetCiaTitleId(argv[i]) == 0x0004000e0ffac000ULL,
                        "Update CIA classification failed");
                FileUtil::IOFile update;
                Require(!update.OpenRom(argv[i]), "Update accepted as a launchable game");
                continue;
            }
            if (path == "--bad") {
                invalid = true;
                continue;
            }
            FileUtil::IOFile first;
            if (invalid) {
                Require(!first.OpenRom(argv[i]), "Malformed or ambiguous compressed ROM accepted");
                continue;
            }
            Require(first.OpenRom(argv[i]) && first.GetSize() == expected.size(),
                    "Compressed ROM open failed for " + std::string(path));
            FileUtil::IOFile other;
            Require(other.OpenRom(argv[i]), "Compressed ROM reopen failed");
            FileUtil::IOFile raw_archive(argv[i], "rb");
            std::array<u8, 4> signature{};
            Require(raw_archive.ReadBytes(signature.data(), signature.size()) == signature.size() &&
                        (signature == std::array<u8, 4>{0x20, 0x20, 0, 0} ||
                         signature == std::array<u8, 4>{0x50, 0x4b, 3, 4} ||
                         signature == std::array<u8, 4>{0x37, 0x7a, 0xbc, 0xaf} ||
                         signature == std::array<u8, 4>{'Z', '3', 'D', 'S'}),
                    "Plain archive open returned ROM content");
            const u64 boundary = 1024 * 1024;
            for (u64 offset : {u64{0}, boundary - 16, boundary, 3 * boundary + 5,
                               u64{expected.size() - 32}}) {
                Compare(first, expected, offset, 32);
                Compare(other, expected, offset / 2, 49);
                Require(first.Tell() == offset + 32, "Reopen shares the active read cursor");
            }
            std::mt19937 generator(20261121);
            for (unsigned j = 0; j < 128; ++j) {
                const auto offset = generator() % (expected.size() - 512);
                Compare(j % 2 ? first : other, expected, offset, 512);
            }
            Compare(first, expected, boundary + 19, 31);
            std::stringstream state;
            {
                boost::archive::binary_oarchive archive(state);
                archive << first;
            }
            FileUtil::IOFile restored;
            {
                boost::archive::binary_iarchive archive(state);
                archive >> restored;
            }
            Require(restored.Tell() == boundary + 50 &&
                        restored.GetSize() == expected.size(),
                    "Serialized ROM handle reopened with the wrong mode or cursor");
            Compare(restored, expected, 2 * boundary + 17, 173);
            Require(first.Seek(-8, SEEK_END) && first.Tell() == expected.size() - 8,
                    "Relative end seek failed");
            std::array<u8, 16> tail{};
            Require(first.ReadArray(tail.data(), tail.size()) == 8 &&
                        std::equal(tail.begin(), tail.begin() + 8, expected.end() - 8),
                    "Short read at EOF differed");
            Require(!first.Seek(-1, SEEK_SET), "Negative absolute seek accepted");
            ++archives;
        }
        std::cout << "PASS " << archives << " compressed ROM variants with byte-exact independent random reads\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
