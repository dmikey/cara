// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <chrono>
#include <string>
#include <discord_rpc.h>
#include "cara_qt/discord_impl.h"
#include "cara_qt/uisettings.h"
#include "common/common_types.h"
#include "core/core.h"

namespace DiscordRPC {

DiscordImpl::DiscordImpl() {
    DiscordEventHandlers handlers{};

    // The number is the client ID for Cara, it's used for images and the
    // application name
    Discord_Initialize("461729900748079114", &handlers, 1, nullptr);
}

DiscordImpl::~DiscordImpl() {
    Discord_ClearPresence();
    Discord_Shutdown();
}

void DiscordImpl::Pause() {
    Discord_ClearPresence();
}

void DiscordImpl::Update() {
    s64 start_time = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    std::string title;
    if (Core::System::GetInstance().IsPoweredOn())
        Core::System::GetInstance().GetAppLoader().ReadTitle(title);
    DiscordRichPresence presence{};
    presence.largeImageKey = "cara_logo";
    presence.largeImageText = "Cara is an emulator for the Nintendo 3DS";
    if (Core::System::GetInstance().IsPoweredOn()) {
        presence.state = title.c_str();
        presence.details = "Currently in game";
    } else {
        presence.details = "Not in game";
    }
    presence.startTimestamp = start_time;
    Discord_UpdatePresence(&presence);
}
} // namespace DiscordRPC
