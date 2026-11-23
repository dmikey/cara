// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_pipelines.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include <cmath>
#include <iostream>
#include <stdexcept>
void Check(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
int main() { @autoreleasepool { try {
    setenv("CARA_METAL_FRAME_COST","1",1);
    auto device=MTLCreateSystemDefaultDevice(); Check(device && device.hasUnifiedMemory,"Apple GPU unavailable");
    Cara::PicaMetalPipelines pipelines(device);
    std::string vertex="//"+std::string(340000,'v')+"\nvoid main() {\n"
        "vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));\n"
        "gl_Position=vec4(p[gl_VertexID],0.0,1.0);\n}\n";
    Cara::PicaPipelineInputs p{};
    p.vertex=vertex; p.vertex_mode=Cara::PicaMetalVertexMode::StageIn;
    p.sampled_unit=Cara::NoMaterialUnit; p.clip0=true; p.dither=false;
    p.color_mask=15; p.winding=0x900;
    Cara::PicaSampleInputs sample{};
    auto render=[&](const Cara::PicaMetalPipelineSelection& selection,unsigned red) {
        auto queue=[device newCommandQueue];
        auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
        desc.storageMode=MTLStorageModeShared; desc.usage=MTLTextureUsageRenderTarget;
        auto color=[device newTextureWithDescriptor:desc];
        desc.pixelFormat=MTLPixelFormatDepth32Float_Stencil8; desc.storageMode=MTLStorageModePrivate;
        auto depth=[device newTextureWithDescriptor:desc];
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=color; pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.depthAttachment.texture=depth; pass.stencilAttachment.texture=depth;
        auto command=[queue commandBuffer]; auto encoder=[command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:selection.pipeline]; [encoder setDepthStencilState:selection.depth];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        Check(command.status==MTLCommandBufferStatusCompleted && !command.error,"Pooled runtime pipeline GPU execution failed");
        std::array<unsigned char,256> pixels{};
        [color getBytes:pixels.data() bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
        for (unsigned i=0;i<pixels.size();i+=4)
            Check(pixels[i]==red && pixels[i+1]==64 && pixels[i+2]==128 && pixels[i+3]==255,
                  "Pooled runtime pipeline changed fragment source/pixels");
    };
    for (unsigned i=0;i<220;++i) {
        std::string fragment="layout(location=0) out vec4 color; void main() { color=vec4("+
            std::to_string(i)+".0/220.0,0.25,0.5,1.0); }\n";
        p.fragment=fragment;
        auto units=pipelines.GateMaterialUnits(vertex,fragment,p.vertex_mode);
        Check(units && *units==std::array<unsigned,3>{3,3,3},"Repeated runtime stage admission exhausted pair budget");
        if (i==0 || i==219) {
            Check(pipelines.Prepare(p,sample),"Preparing pooled runtime pipeline failed");
            auto selection=pipelines.Select(p,sample); Check(bool(selection),"Selecting pooled runtime pipeline failed");
            render(*selection,std::lround(float(i)/220*255));
        }
    }
    const auto& spans=Cara::NativeMetalCost::spans;
    auto logical=spans[Cara::NativeMetalCost::RuntimeSourceLogicalBytes].calls;
    auto unique=spans[Cara::NativeMetalCost::RuntimeSourceUniqueBytes].calls;
    Check(logical>64*1024*1024 && unique<64*1024*1024,"Runtime source accounting did not remove repeated stage ownership");
    std::cout << "PASS:220 production runtime pairs, " << logical << " logical bytes / " << unique
              << " owned UTF-8 bytes, first/final GPU pixels exact\n";
    return 0;
} catch (const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
} }
