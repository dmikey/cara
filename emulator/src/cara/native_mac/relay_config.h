// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once
#include <string_view>
#include "network/relay_client.h"
namespace NativeMac {
// Strict, bounded private input. No diagnostics may include input contents.
bool ParseRelayConfig(std::string_view input, Network::RelayClient::Config& config);
}
