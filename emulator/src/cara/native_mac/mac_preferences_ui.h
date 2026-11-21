// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#pragma once
#include <optional>
#include <string>
#include "cara/native_mac/play_settings.h"
namespace Cara::MacUI {
std::string Welcome(); // No Core or game load. Empty means cancel/quit.
bool HandleStartupOpen();
void ShowPreferences();
std::optional<MacPreferences::PlaySettings> TakeSavedPlaySettings();
bool TakePreferencesClosed();
int SelfTest(const std::string& output_directory);
}
