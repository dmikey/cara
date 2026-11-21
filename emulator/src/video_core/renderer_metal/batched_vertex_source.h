// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Cara {

// Accept the packed SPIRV-Cross ABI only. The raster entry and the compute
// entry share the complete original shader body, including the reference
// vertex, clip conversion, viewport snap and all varyings.
std::optional<std::string> MakeBatchedVertexSource(std::string_view vertex,
                                                   bool gather_unique_vertices = false);

// Accept only the generated packed program whose reference and actual vertex
// both execute the same PICA entry, with identical quaternion output expressions.
std::optional<std::string> MakeUniquePackedVertexSource(std::string_view vertex);

} // namespace Cara
