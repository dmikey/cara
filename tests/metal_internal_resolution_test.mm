// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_lcd_native.h"
#include "video_core/renderer_metal/pica_metal_resolution.h"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
void Check(bool ok, const char* error) {
    if (!ok) throw std::runtime_error(error);
}

id<MTLTexture> Texture(id<MTLDevice> device, unsigned w, unsigned h, MTLPixelFormat format) {
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    d.storageMode = MTLStorageModePrivate;
    return [device newTextureWithDescriptor:d];
}

id<MTLBuffer> Read(id<MTLCommandBuffer> command, id<MTLTexture> texture, unsigned pixel_bytes,
                   MTLBlitOption option = MTLBlitOptionNone) {
    const auto pitch = (texture.width*pixel_bytes+255)&~NSUInteger{255};
    auto bytes = [command.device newBufferWithLength:pitch*texture.height options:MTLResourceStorageModeShared];
    auto blit = [command blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(texture.width,texture.height,1) toBuffer:bytes destinationOffset:0
        destinationBytesPerRow:pitch destinationBytesPerImage:pitch*texture.height options:option];
    [blit endEncoding];
    return bytes;
}

void Run(const char* file) {
    auto device = MTLCreateSystemDefaultDevice();
    auto queue = [device newCommandQueue];
    NSError* error = nil;
    auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(file)] error:&error];
    Check(library!=nil, "missing fixture metallib");
    Cara::MetalRasterScale scale(library,2);
    auto lcd = Cara::MetalLCD::PreparePipelines(library,MTLPixelFormatRGBA8Unorm,&error);
    Check(lcd.has_value(), "LCD pipelines");
    const unsigned w=240, h=400;
    auto native=Texture(device,w,h,MTLPixelFormatRGBA8Unorm);
    auto depth=Texture(device,w,h,MTLPixelFormatDepth32Float_Stencil8);
    auto output=Texture(device,2*h,2*w,MTLPixelFormatRGBA8Unorm);
    auto gen=[device newLibraryWithSource:@R"(
#include <metal_stdlib>
using namespace metal;
fragment float4 checker(float4 p [[position]]) {
    const float v=float((uint(p.x)^uint(p.y))&1u); return float4(v,v,v,1);
})" options:nil error:&error];
    auto pd=[MTLRenderPipelineDescriptor new];
    pd.vertexFunction=[library newFunctionWithName:@"cara_scale_vertex"];
    pd.fragmentFunction=[gen newFunctionWithName:@"checker"];
    pd.colorAttachments[0].pixelFormat=native.pixelFormat;
    pd.depthAttachmentPixelFormat=pd.stencilAttachmentPixelFormat=depth.pixelFormat;
    auto pipe=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    Check(pipe!=nil, "checker pipeline");
    for (int run=0;run<8;++run) { @autoreleasepool {
        auto command=[queue commandBuffer];
        scale.Invalidate();
        auto clear=[MTLRenderPassDescriptor renderPassDescriptor];
        clear.colorAttachments[0].texture=native;
        clear.depthAttachment.texture=clear.stencilAttachment.texture=depth;
        clear.colorAttachments[0].loadAction=clear.depthAttachment.loadAction=clear.stencilAttachment.loadAction=MTLLoadActionClear;
        clear.colorAttachments[0].storeAction=clear.depthAttachment.storeAction=clear.stencilAttachment.storeAction=MTLStoreActionStore;
        clear.depthAttachment.clearDepth=.25; clear.stencilAttachment.clearStencil=0xa1;
        [[command renderCommandEncoderWithDescriptor:clear] endEncoding];
        Check(scale.Begin(command,native,depth,true), "2x attachment expansion");
        const auto hi=scale.Target();
        Check(hi.color.width==2*w && hi.color.height==2*h, "2x raster dimensions");
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=hi.color;
        pass.depthAttachment.texture=pass.stencilAttachment.texture=hi.depth;
        pass.colorAttachments[0].loadAction=pass.depthAttachment.loadAction=pass.stencilAttachment.loadAction=MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction=pass.depthAttachment.storeAction=pass.stencilAttachment.storeAction=MTLStoreActionStore;
        auto enc=[command renderCommandEncoderWithDescriptor:pass];
        [enc setRenderPipelineState:pipe]; [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        Check(scale.Resolve(command), "guest resolve");
        auto image=scale.SnapshotLinear(command,native,w,h,3,true);
        Check(image!=nil,"linear RGB scanout snapshot");
        std::array<std::optional<Cara::MetalLCD::Screen>,2> screens{};
        screens[0]=Cara::MetalLCD::Screen{image,false,{},std::nullopt,true};
        Layout::FramebufferLayout layout{2*h,2*w,true,false,{0,0,2*h,2*w},{}};
        Check(Cara::MetalLCD::EncodeComposition(command,output,layout,*lcd,
            Cara::MetalLCD::Filter::Nearest,Cara::MetalLCD::EdgeMode::ClampToEdge,screens), "LCD composition");
        auto guest=Read(command,native,4);
        auto guest_z=Read(command,depth,4,MTLBlitOptionDepthFromDepthStencil);
        auto guest_s=Read(command,depth,1,MTLBlitOptionStencilFromDepthStencil);
        auto pixels=Read(command,output,4);
        [command commit];[command waitUntilCompleted];
        if (command.error) NSLog(@"%@",command.error);
        Check(command.status==MTLCommandBufferStatusCompleted,"GPU completion");
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
            auto* color=static_cast<const unsigned char*>(guest.contents)+1024*y+4*x;
            Check(color[0]==128 && color[3]==255,"guest color did not resolve");
            Check(static_cast<const float*>(guest_z.contents)[256*y+x]==.25f,"guest depth changed");
            Check(static_cast<const unsigned char*>(guest_s.contents)[256*y+x]==0xa1,"guest stencil changed");
        }
        for(unsigned y=0;y<2*w;++y)for(unsigned x=0;x<2*h;++x) {
            const auto pixel=static_cast<const unsigned char*>(pixels.contents)+3328*y+4*x;
            Check(pixel[0]==(((x^y)&1u)?255:0),"LCD lost subpixel contrast");
        }
        printf("run=%d gpu_ms=%.3f guest=240x400 lcd=800x480\n",run,
               1000*(command.GPUEndTime-command.GPUStartTime));
    }}
}
}
int main(int argc,char** argv) {
    @autoreleasepool {
        if (argc!=2) return 2;
        try { Run(argv[1]); } catch (const std::exception& e) { fprintf(stderr,"%s\n",e.what()); return 1; }
    }
}
