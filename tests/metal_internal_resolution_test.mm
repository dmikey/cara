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
    const float v=float((uint(p.x)^uint(p.y))&1u);
    return float4(v,float(uint(p.y)&255u)/255.f,float(uint(p.x)&255u)/255.f,1);
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
        constexpr unsigned crop_w = 224, crop_h = 360, first_row = 24;
        std::array<id<MTLBuffer>, 2> crops{}, packed_crops{};
        for (unsigned flip = 0; flip < crops.size(); ++flip) {
            packed_crops[flip] = [device newBufferWithLength:crop_w*crop_h*2 options:MTLResourceStorageModeShared];
            auto crop = scale.SnapshotRgb565Crop(command,native,crop_w,crop_h,first_row,flip,packed_crops[flip]);
            Check(crop && crop.width == 2*crop_w && crop.height == 2*crop_h, "RGB565 crop bounds");
            crops[flip] = Read(command, crop, 4);
        }
        Check(!scale.SnapshotRgb565Crop(command,native,crop_w,crop_h,h-crop_h+1,false,packed_crops[0]),
              "crop outside resident height");
        Check(!scale.SnapshotRgb565Crop(command,native,w+1,crop_h,0,false,packed_crops[0]),
              "crop outside resident width");
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
        const auto quantized = [](unsigned value, unsigned levels) {
            const unsigned packed = (value * levels + 127) / 255;
            return (packed * 255 + levels/2) / levels;
        };
        for (unsigned flip = 0; flip < crops.size(); ++flip) {
            constexpr unsigned pitch = ((crop_w*2*4 + 255)/256)*256;
            for (unsigned y = 0; y < crop_h*2; ++y) {
                const unsigned guest_y = first_row*2 + (flip ? crop_h*2-1-y : y);
                const unsigned source_y = h*2-1-guest_y;
                const auto* words = static_cast<const uint16_t*>(packed_crops[flip].contents);
                for (unsigned x = 0; x < crop_w*2; ++x) {
                    if ((x % 2 == 0) && (y % 2 == 0)) {
                        const auto* guest_pixel = static_cast<const uint8_t*>(guest.contents) +
                            1024*(source_y/2) + 4*(x/2);
                        const unsigned expected = (((guest_pixel[0]*31u+127)/255) << 11) |
                            (((guest_pixel[1]*63u+127)/255) << 5) | ((guest_pixel[2]*31u+127)/255);
                        Check(words[(y/2)*crop_w + x/2] == expected, "incorrect native RGB565 guest word");
                    }
                    const auto* p = static_cast<const uint8_t*>(crops[flip].contents) + pitch*y + 4*x;
                    Check(p[0] == (((x ^ source_y)&1) ? 255 : 0) &&
                          p[1] == quantized(source_y&255u,63) && p[2] == quantized(x&255u,31) &&
                          p[3] == 255, "incorrect RGB565 crop pixel or row orientation");
                }
            }
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
