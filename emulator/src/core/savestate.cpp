// Copyright 2020 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <cryptopp/sha.h>
#include <zstd.h>
#include "cara_state_version.h"
#include "common/archives.h"
#include "common/file_util.h"
#include "core/core.h"
#include "core/savestate.h"
#include "core/settings.h"
#include "network/network.h"

namespace Core {
namespace {
constexpr std::array<u8, 8> magic{'C','A','R','A','S','T','0','1'};
constexpr u64 max_archive_size = 768ull * 1024 * 1024;
#pragma pack(push, 1)
struct Header {
    std::array<u8, 8> magic_bytes;
    u32 version = 1;
    u32 header_size = 256;
    u64 title = 0;
    u64 timestamp = 0;
    u64 compressed_size = 0;
    u64 archive_size = 0;
    std::array<char, 64> engine{};
    std::array<u8, 32> digest{};
    u32 new_3ds = 0;
    std::array<u8, 32> game_hash{};
    u32 cpu_backend = 0;
    std::array<u8, 72> reserved{};
};
#pragma pack(pop)
static_assert(sizeof(Header) == 256);

std::array<u8, 32> Digest(const u8* bytes, std::size_t size) {
    std::array<u8, 32> digest;
    CryptoPP::SHA256().CalculateDigest(digest.data(), bytes, size);
    return digest;
}
std::string StatePath(u64 title, u32 slot, const std::array<u8, 32>* game_hash = nullptr) {
    if (slot < 1 || slot > SaveStateSlotCount) throw std::runtime_error("Invalid save-state slot");
    if (!title && game_hash) {
        std::string identity;
        for (auto byte : *game_hash) identity += fmt::format("{:02X}", byte);
        return fmt::format("{}homebrew-{}.{:02d}.carastate", FileUtil::GetUserPath(FileUtil::UserPath::StatesDir), identity, slot);
    }
    return fmt::format("{}{:016X}.{:02d}.carastate", FileUtil::GetUserPath(FileUtil::UserPath::StatesDir), title, slot);
}
u32 CPUBackend() { const char* backend = std::getenv("CARA_CPU_BACKEND"); return backend && std::strcmp(backend, "dyncom") == 0 ? 1 : 0; }
void CheckOffline() {
    if (auto room = Network::GetRoomMember().lock(); room && room->IsConnected())
        throw std::runtime_error("Disconnect multiplayer before using save states");
}
std::string Capture(const System& system) {
    std::fflush(nullptr);
    std::ostringstream stream(std::ios::binary);
    { oarchive archive(stream); archive & system; }
    return std::move(stream).str();
}
void Restore(System& system, const std::string& bytes) {
    std::istringstream stream(bytes, std::ios::binary);
    iarchive archive(stream);
    archive & system;
    if (stream.bad()) throw std::runtime_error("Unable to read save-state archive");
}
std::vector<u8> Compress(const std::string& bytes) {
    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(), ZSTD_freeCCtx);
    if (!context || ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, 1)) ||
        ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(context.get(), bytes.size())))
        throw std::runtime_error("Unable to initialize save-state compression");
    ZSTD_inBuffer input{bytes.data(), bytes.size(), 0};
    std::vector<u8> chunk(ZSTD_CStreamOutSize()), compressed;
    size_t remaining;
    do {
        ZSTD_outBuffer output{chunk.data(), chunk.size(), 0};
        remaining=ZSTD_compressStream2(context.get(), &output, &input, ZSTD_e_end);
        if (ZSTD_isError(remaining) || compressed.size()+output.pos>max_archive_size)
            throw std::runtime_error("Unable to compress save state within the supported size");
        compressed.insert(compressed.end(), chunk.begin(), chunk.begin()+output.pos);
    } while (remaining);
    return compressed;
}

Header ReadHeader(FileUtil::IOFile& file) {
    Header header;
    if (file.GetSize() < sizeof(header) || file.ReadBytes(&header, sizeof(header)) != sizeof(header))
        throw std::runtime_error("Save-state file is incomplete");
    if (header.magic_bytes != magic || header.version != 1 || header.header_size != sizeof(header))
        throw std::runtime_error("Not a supported Cara save state");
    if (!header.compressed_size || !header.archive_size || header.archive_size > max_archive_size ||
        header.compressed_size > max_archive_size || file.GetSize() != sizeof(header) + header.compressed_size)
        throw std::runtime_error("Save-state size is invalid");
    return header;
}
}

std::vector<SaveStateInfo> ListSaveStates(u64 title) {
    std::vector<SaveStateInfo> states;
    for (u32 slot = 1; slot <= SaveStateSlotCount; ++slot) {
        const auto path = StatePath(title, slot);
        if (!FileUtil::Exists(path)) continue;
        try {
            FileUtil::IOFile file(path, "rb");
            if (!file.IsOpen()) continue;
            const auto header = ReadHeader(file);
            if (header.title != title) continue;
            SaveStateInfo info{}; info.slot = slot; info.time = header.timestamp;
            info.status = std::memcmp(header.engine.data(), CARA_STATE_FINGERPRINT, 64) == 0
                ? SaveStateInfo::ValidationStatus::OK : SaveStateInfo::ValidationStatus::RevisionDismatch;
            states.push_back(info);
        } catch (const std::exception&) { /* An incomplete file is not a usable slot. */ }
    }
    return states;
}

void System::SaveState(u32 slot) const { SaveStateToPath(StatePath(GetTitleID(), slot, &state_game_hash)); }
void System::LoadState(u32 slot) { LoadStateFromPath(StatePath(GetTitleID(), slot, &state_game_hash)); }

void System::SaveStateToPath(const std::string& path) const {
    if (!IsPoweredOn()) throw std::runtime_error("Open a game before saving a state");
    CheckOffline();
    const auto bytes = Capture(*this);
    if (bytes.empty() || bytes.size() > max_archive_size) throw std::runtime_error("Save state exceeds the supported size");
    const auto compressed = Compress(bytes);
    if (compressed.size() > max_archive_size) throw std::runtime_error("Compressed save state exceeds the supported size");
    Header header{}; header.magic_bytes = magic; header.title = GetTitleID();
    header.timestamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    header.compressed_size = compressed.size(); header.archive_size = bytes.size();
    header.new_3ds = Settings::values.is_new_3ds;
    header.game_hash = StateGameHash();
    header.cpu_backend = CPUBackend();
    std::memcpy(header.engine.data(), CARA_STATE_FINGERPRINT, 64);
    header.digest = Digest(compressed.data(), compressed.size());
    if (!FileUtil::CreateFullPath(path)) throw std::runtime_error("Unable to create save-state directory");
    const auto temporary = path + ".tmp";
    try {
        FileUtil::IOFile file(temporary, "wb");
        if (!file.IsOpen() || file.WriteBytes(&header, sizeof(header)) != sizeof(header) ||
            file.WriteBytes(compressed.data(), compressed.size()) != compressed.size() || !file.Flush())
            throw std::runtime_error("Unable to write save state");
        file.Close();
        if (std::rename(temporary.c_str(), path.c_str()) != 0) throw std::runtime_error("Unable to commit save state");
    } catch (...) { FileUtil::Delete(temporary); throw; }
}

void System::LoadStateFromPath(const std::string& path) {
    if (!IsPoweredOn()) throw std::runtime_error("Open the matching game before loading a state");
    CheckOffline();
    FileUtil::IOFile file(path, "rb");
    if (!file.IsOpen()) throw std::runtime_error("No save state exists in this slot");
    const auto header = ReadHeader(file);
    if (header.title != GetTitleID() || header.game_hash != StateGameHash()) throw std::runtime_error("Save state belongs to another game or game revision");
    if (std::memcmp(header.engine.data(), CARA_STATE_FINGERPRINT, 64) != 0)
        throw std::runtime_error("Save state was created by an incompatible engine build");
    if (header.cpu_backend != CPUBackend()) throw std::runtime_error("Save state uses a different CPU backend");
    if (bool(header.new_3ds) != Settings::values.is_new_3ds)
        throw std::runtime_error("Save state uses a different 3DS system mode");
    std::vector<u8> compressed(header.compressed_size);
    if (file.ReadBytes(compressed.data(), compressed.size()) != compressed.size()) throw std::runtime_error("Unable to read save state");
    file.Close();
    if (Digest(compressed.data(), compressed.size()) != header.digest) throw std::runtime_error("Save-state checksum failed");
    const auto frame_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (frame_size != header.archive_size) throw std::runtime_error("Save-state compression size is invalid");
    std::string incoming(header.archive_size, '\0');
    const auto result = ZSTD_decompress(incoming.data(), incoming.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(result) || result != incoming.size()) throw std::runtime_error("Unable to decompress save state");
    compressed.clear(); compressed.shrink_to_fit();
    // Retain a rollback snapshot before applying an archive that may fail to restore.
    const auto previous = Capture(*this);
    try { Restore(*this, incoming); }
    catch (...) {
        const auto failure = std::current_exception();
        try { Restore(*this, previous); }
        catch (...) { if (IsPoweredOn()) Shutdown(); throw std::runtime_error("State restore failed; reopen the game to continue"); }
        std::rethrow_exception(failure);
    }
}
}
