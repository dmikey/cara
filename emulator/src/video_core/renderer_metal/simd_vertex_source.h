// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace Cara {
inline constexpr std::string_view SimdVertexTag="\n// CARA_METAL_VERTEX_SIMD_REFERENCE_V1\n";
inline constexpr std::string_view SimdVertexMslTag="\n// CARA_METAL_VERTEX_SIMD_MSL_V1\n";
bool HasSimdVertexTag(std::string_view source);
std::string StripSimdVertexTag(std::string_view source);
std::optional<std::string> RemoveVertexReference(std::string_view source);
bool TagSimdVertexSource(std::string& source);
// diagnostic=true adds output buffers4/5 for tests only; production uses slot3 only.
std::optional<std::string> MakeVertexReferenceSource(std::string_view msl,bool simd,bool diagnostic=false);
bool ValidSimdVertexMsl(std::string_view msl);
}
