// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once
#include <string>
namespace Core { class System; }
namespace Cara::NativeBehavior {
struct Result { bool passed=false; std::string error; std::string detail_json; };
// Exact original bootstrap guard runs before System::Load, never accepting another executable.
bool ValidateBootstrap(const std::string& path,std::string& error);
Result RunOneDraw(Core::System& system, bool software=false, bool indexed=false);
} // namespace Cara::NativeBehavior
