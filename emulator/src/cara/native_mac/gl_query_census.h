// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once
#include <string>
#include <glad/glad.h>

namespace Cara::NativeGLQueryCensus {
// Native context owner only. No GL calls are added by this diagnostic.
void Install(bool measure_thread_cpu);
void Uninstall();
bool AuditConnected(PFNGLGETERRORPROC expected);
void WriteReport(const std::string& path, const std::string& source_fingerprint);
} // namespace Cara::NativeGLQueryCensus
