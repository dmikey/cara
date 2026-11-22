// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#include "video_core/renderer_metal/pica_metal_material_filter.h"
#include <cmath>
#include <initializer_list>
#include <cstdio>

int main() {
    @autoreleasepool {
        auto device = MTLCreateSystemDefaultDevice();
        NSError* error = nil;
        auto library = [device newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void sample_pair(texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]], "
             "device float* out [[buffer(0)]]) { *out = tex.sample(s, float2(0.375, 0.5), level(0)).r; }"
             options:nil error:&error];
        auto pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"sample_pair"]
                                                              error:&error];
        if (!pipeline) { NSLog(@"%@", error); return 1; }
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                             width:2 height:1 mipmapped:NO];
        auto texture = [device newTextureWithDescriptor:descriptor];
        const unsigned char pixels[] = {0, 255};
        [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0 withBytes:pixels bytesPerRow:2];
        auto queue = [device newCommandQueue];
        auto result = [device newBufferWithLength:sizeof(float) options:MTLResourceStorageModeShared];
        for (auto mode : {Cara::MetalTextureFilter::Game, Cara::MetalTextureFilter::Nearest,
                          Cara::MetalTextureFilter::Linear}) {
            auto sampling = [MTLSamplerDescriptor new];
            sampling.minFilter = sampling.magFilter = MTLSamplerMinMagFilterLinear;
            Cara::ApplyMaterialFilter(sampling, mode);
            auto sampler = [device newSamplerStateWithDescriptor:sampling];
            auto command = [queue commandBuffer];
            auto encoder = [command computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setTexture:texture atIndex:0];
            [encoder setSamplerState:sampler atIndex:0];
            [encoder setBuffer:result offset:0 atIndex:0];
            [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted) return 1;
            const float value = *static_cast<float*>(result.contents);
            const float expected = mode == Cara::MetalTextureFilter::Nearest ? 0.f : 0.25f;
            if (!std::isfinite(value) || std::abs(value - expected) > 0.005f) return 1;
            std::printf("mode %d sampled %.3f\n", int(mode), value);
        }
    }
}
