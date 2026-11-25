// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include "common/file_util.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hw/gpu.h"
#include "core/hw/lcd.h"
#include "core/memory.h"
#include "video_core/video_core.h"

namespace Cara::NativeLCDMaterialize {
inline bool Enabled() {
#if defined(CARA_MAC_CPU_JIT)
    static const bool enabled=[]{
        const auto* opt=std::getenv("CARA_NATIVE_LCD_MATERIALIZE");
        const auto* diag=std::getenv("CARA_NATIVE_DIAGNOSTICS");
        const auto* ledger=std::getenv("CARA_NATIVE_REPLAY_LEDGER");
        return opt&&diag&&ledger&&std::strcmp(opt,"2400")==0&&
               std::strcmp(diag,"1")==0&&std::strcmp(ledger,"1")==0;
    }();
    return enabled;
#else
    return false;
#endif
}

// Called only after the selected frame's normal compositor has sampled its
// scanout, before FrameUpdate advances guest state. This diagnostic explicitly
// materializes GPU-authoritative guest bytes through the existing cache API.
// It is separate from timing measurements and not enabled in normal play.
inline void AtCapture(std::uint32_t frame) {
    if(!Enabled()||frame!=2400)return;
    if(VideoCore::GetCurrentFrame()+1!=frame)
        throw std::runtime_error("LCD materialization missed the frame boundary");
    static bool done=false;
    if(done)throw std::runtime_error("Duplicate LCD materialization");
    done=true;
    const auto directory=FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
    const auto stem=directory+"native-lcd-materialized-2400";
    auto& timing=Core::System::GetInstance().CoreTiming();
    std::ofstream meta(stem+".json");
    meta<<"{\"refresh\":"<<frame<<",\"ticks\":"<<timing.GetTicks()
        <<",\"global_ticks\":"<<timing.GetGlobalTicks()<<",\"panels\":[";
    for(unsigned screen=0;screen<2;++screen){
        const auto& fb=GPU::g_regs.framebuffer_config[screen];
        const auto fill=screen?LCD::g_regs.color_fill_bottom:LCD::g_regs.color_fill_top;
        const u32 w=screen?320:400,h=240;
        const auto format=fb.color_format.Value();
        const u32 bpp=static_cast<u32>(format)<=4?GPU::Regs::BytesPerPixel(format):0;
        const PAddr address=fb.active_fb?fb.address_left2:fb.address_left1;
        const u64 span=u64(w-1)*fb.stride+u64(h)*bpp,end=u64(address)+span;
        const bool valid=bpp&&fb.width>=h&&fb.height>=w&&fb.stride>=h*bpp&&span<=1024*1024&&
            ((address>=Memory::VRAM_PADDR&&end<=Memory::VRAM_PADDR_END)||
             (address>=Memory::FCRAM_PADDR&&end<=Memory::FCRAM_N3DS_PADDR_END));
        const bool filled=fill.is_enabled;
        if(!filled){
            if(!valid)throw std::runtime_error("Active LCD guest span invalid");
            Memory::RasterizerFlushRegion(address,static_cast<u32>(span));
            const auto ref=VideoCore::Memory()->GetPhysicalRef(address);
            if(!ref||!ref.GetPtr()||ref.GetSize()<span)
                throw std::runtime_error("Active LCD guest bytes inaccessible");
            std::ofstream raw(stem+"-"+std::to_string(screen)+".bin",std::ios::binary);
            raw.write(reinterpret_cast<const char*>(ref.GetPtr()),static_cast<std::streamsize>(span));
            raw.close();
            if(!raw)throw std::runtime_error("Could not retain materialized LCD bytes");
        }
        meta<<(screen?",":"")<<"{\"panel\":"<<screen<<",\"width\":"<<w<<",\"height\":"<<h
            <<",\"address\":"<<address<<",\"active_fb\":"<<fb.active_fb
            <<",\"fb_width\":"<<fb.width<<",\"fb_height\":"<<fb.height
            <<",\"stride\":"<<fb.stride<<",\"format\":"<<static_cast<u32>(format)
            <<",\"pixel_bytes\":"<<bpp<<",\"span_bytes\":"<<(filled?0:span)
            <<",\"fill_raw\":"<<fill.raw<<",\"fill_enabled\":"<<(filled?"true":"false")
            <<",\"fill_rgb\":["<<unsigned(fill.color_r)<<','<<unsigned(fill.color_g)
            <<','<<unsigned(fill.color_b)<<"]}";
    }
    meta<<"]}\n";meta.close();
    if(!meta)throw std::runtime_error("Could not retain active LCD metadata");
}
}
