// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_binding_names.h"

int main() {
    @autoreleasepool {
        if (@available(macOS 13.0, *)) {
            auto device = MTLCreateSystemDefaultDevice();
            NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
                "vertex float4 vs(uint id [[vertex_id]]) { return float4(id,0,0,1); }\n"
                "fragment float4 fs(texture2d<float> tex0 [[texture(0)]],"
                "texture2d<float> tex1 [[texture(1)]], sampler tex0Smplr [[sampler(0)]],"
                "sampler tex1Smplr [[sampler(1)]]) { return tex0.sample(tex0Smplr,float2(0.5)); }";
            auto library = [device newLibraryWithSource:source options:nil error:nil];
            auto desc = [MTLRenderPipelineDescriptor new];
            desc.vertexFunction = [library newFunctionWithName:@"vs"];
            desc.fragmentFunction = [library newFunctionWithName:@"fs"];
            desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            MTLRenderPipelineReflection* reflection = nil;
            auto pipeline = [device newRenderPipelineStateWithDescriptor:desc
                options:MTLPipelineOptionBindingInfo reflection:&reflection error:nil];
            if (!pipeline || !reflection)
                return 1;
            const auto bindings = reflection.fragmentBindings;
            return !(Cara::MatchesMaterialBinding(bindings, MTLBindingTypeTexture, 0, @"tex0", true) &&
                     !Cara::MatchesMaterialBinding(bindings, MTLBindingTypeTexture, 0, @"tex1", true) &&
                     Cara::MatchesMaterialBinding(bindings, MTLBindingTypeSampler, 1, @"tex1Smplr", true) &&
                     !Cara::MatchesMaterialBinding(bindings, MTLBindingTypeSampler, 1, @"tex1Smplr", false));
        }
        return 1;
    }
}
