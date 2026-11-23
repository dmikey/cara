// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_render_pipeline_key.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char* detail) {
    if (!condition) {
        std::cerr << detail << '\n';
        std::exit(1);
    }
}
}

int main() {
    // Authored render recipe: shader pair 7, blending, RGBA writes, add,
    // source-one/destination-zero blending, RGBA8 color attachment. The tail
    // represents depth/stencil and each of the three sampler recipes.
    constexpr std::array<unsigned, 28> material{
        7, 1, 15, 0x8006, 0x8006, 1, 0, 1, 0, 70,
        0x201, 1, 1, 0x202, 255, 255, 0x1e00, 0x1e00, 0x1e00,
        0, 0x2901, 0x2901, 0, 0x2901, 0x2901, 0, 0x2901, 0x2901};
    Require(Cara::SamePicaMetalRenderPipeline(material, material), "Identical recipe rejected");

    auto depth_and_sampler_variant = material;
    depth_and_sampler_variant[10] = 0x207; // always instead of less
    depth_and_sampler_variant[11] = 0; // no depth write
    depth_and_sampler_variant[12] = 0; // no stencil
    depth_and_sampler_variant[19] = 1; // primary filter
    depth_and_sampler_variant[23] = 0x812f; // secondary wrap
    depth_and_sampler_variant[27] = 0x812f; // tertiary wrap
    Require(Cara::SamePicaMetalRenderPipeline(material, depth_and_sampler_variant),
            "Separate depth/sampler state unnecessarily changes the render PSO");
    Require(Cara::PicaMetalRenderPrefix(material) ==
                Cara::PicaMetalRenderPrefix(depth_and_sampler_variant),
            "Separate depth/sampler state unnecessarily changes the archive descriptor key");

    for (std::size_t field = 0; field != 10; ++field) {
        auto different_render_descriptor = material;
        ++different_render_descriptor[field];
        Require(!Cara::SamePicaMetalRenderPipeline(material, different_render_descriptor),
                "Changed render-descriptor field aliases an existing pipeline");
        Require(Cara::PicaMetalRenderPrefix(material) !=
                    Cara::PicaMetalRenderPrefix(different_render_descriptor),
                "Changed render-descriptor field aliases an archive descriptor key");
    }
    for (std::size_t field = 10; field != material.size(); ++field) {
        auto separate_binding = material;
        ++separate_binding[field];
        Require(Cara::SamePicaMetalRenderPipeline(material, separate_binding),
                "Separate binding field incorrectly prevents render-pipeline reuse");
    }
    std::cout << "PASS: render PSO identity preserves shader/blend/format distinctions and separates all binding fields\n";
}
