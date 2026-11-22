// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include "common/cia_rom.h"
#include "common/file_util.h"
#include "common/string_util.h"
#include "core/hle/service/am/am.h"

namespace Frontend::Apple {
enum class CiaKind { Game, Update, Unsupported };
struct CiaSelection {
    CiaKind kind;
    u64 title;
    const char* message;
};
inline bool IsCiaPath(const std::string& path) {
    return Common::ToLower(std::filesystem::path(path).extension().string()) == ".cia";
}
inline CiaSelection InspectCia(const std::string& path) {
    const auto id = FileUtil::GetCiaTitleId(path);
    if (!id) return {CiaKind::Unsupported, 0, "The CIA is incomplete or invalid. Copy the original package again."};
    switch (*id >> 32) {
    case 0x00040000: {
        FileUtil::IOFile file;
        if (file.OpenRom(path)) return {CiaKind::Game, *id, nullptr};
        return {CiaKind::Unsupported, *id, "This CIA needs a decrypted, executable base-game content. Open a decrypted dump of the base game."};
    }
    case 0x0004000e: return {CiaKind::Update, *id, nullptr};
    case 0x0004008c: return {CiaKind::Unsupported, *id, "This package contains DLC. Choose the base game or its game update."};
    default: return {CiaKind::Unsupported, *id, "Choose a 3DS base game or its update. This CIA contains another type of title."};
    }
}
inline const char* InstallUpdate(const std::string& path, u64 title) {
    switch (Service::AM::InstallGameUpdate(path, title)) {
    case Service::AM::InstallStatus::Success: return nullptr;
    case Service::AM::InstallStatus::ErrorEncrypted:
        return "Cara could not decrypt the update. Open a decrypted copy of the update.";
    case Service::AM::InstallStatus::ErrorInvalid:
        return "The update has invalid or unsupported game content. Check the original package.";
    default:
        return "Cara could not finish installing this update. Check free storage and file access, then try again.";
    }
}
struct CiaOpenResult {
    bool success;
    const char* message; // Null when the frontend should continue booting the game.
};
inline CiaOpenResult OpenCia(const std::string& path) {
    if (!IsCiaPath(path)) return {true, nullptr};
    const auto result = InspectCia(path);
    if (result.kind == CiaKind::Game) return {true, nullptr};
    if (result.kind == CiaKind::Unsupported) return {false, result.message};
    if (auto message = InstallUpdate(path, result.title)) return {false, message};
    return {true, "Game update installed. Open the matching base game to play with the update."};
}
} // namespace Frontend::Apple
