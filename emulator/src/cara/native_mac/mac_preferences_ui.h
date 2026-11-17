// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#pragma once
#include <string>
namespace Cara::MacUI {
std::string Welcome(); // No Core or game load. Empty means cancel/quit.
bool HandleStartupOpen();
void ShowPreferences();
int SelfTest(const std::string& output_directory);
}
