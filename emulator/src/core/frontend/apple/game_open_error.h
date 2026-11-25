// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <fstream>

#include "common/string_util.h"
#include "common/zip_rom.h"
#include "core/core.h"

namespace Frontend::Apple {

inline const char* GameOpenError(Core::System::ResultStatus status, const std::string& path) {
    using Status = Core::System::ResultStatus;
    switch (status) {
    case Status::ErrorGetLoader: {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) ||
            !std::ifstream(path, std::ios::binary))
            return "Cara couldn’t read the selected file. Check that it is available locally and try again.";
        const auto extension = Common::ToLower(std::filesystem::path(path).extension().string());
        if (extension == ".zip" && FileUtil::IsEncryptedTitleZip(path))
            return "ENCRYPTED DUMP NOT PLAYABLE";
        if (extension == ".zip" || extension == ".7z" || extension == ".z3ds" ||
            extension == ".zcci")
            return "Cara couldn’t read a 3DS cartridge image from this archive. Extract a supported cartridge dump and open it directly.";
        return "Cara couldn’t recognize a playable 3DS game in this file. Choose a cartridge dump, a playable base-game CIA, or a supported homebrew file.";
    }
    case Status::ErrorLoader_ErrorEncrypted:
        return "ENCRYPTED DUMP NOT PLAYABLE";
    case Status::ErrorLoader_ErrorInvalidFormat:
        return "This file is incomplete or has an invalid 3DS format. Copy or dump the original game again.";
    case Status::ErrorSystemMode:
        return "Cara couldn’t read the game’s 3DS system information. Try a complete base-game dump.";
    case Status::ErrorSystemFiles:
        return "Cara couldn’t load the required 3DS system files for this game.";
    case Status::ErrorVideoCore:
    case Status::ErrorVideoCore_ErrorGenericDrivers:
    case Status::ErrorVideoCore_ErrorBelowGL33:
        return "Cara couldn’t start the graphics renderer. Check this device’s graphics support and try again.";
    default:
        return "Cara couldn’t start this game. Try a complete base-game dump and check Cara’s log if the problem continues.";
    }
}

} // namespace Frontend::Apple
