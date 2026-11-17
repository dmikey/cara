// SPDX-License-Identifier: GPL-2.0-or-later
// Original integer-only port/index fixture. Uses actual register bitfield types
// and production decision helper; does not execute GL or WritePicaReg itself.
#include "video_core/lut_effective_change.h"
#include "video_core/regs_lighting.h"
#include "video_core/regs_texturing.h"
#include <array>
#include <cstdlib>
#include <iostream>

using namespace Cara::LutEffectiveChange;
static unsigned checks = 0;
static void Require(bool b, const char* why) {
    ++checks;
    if (!b) { std::cerr << "LUT effective-change: " << why << '\n'; std::exit(1); }
}
struct Model {
    Pica::LightingRegs lighting{};
    Pica::TexturingRegs texturing{};
    std::array<std::array<std::uint32_t,256>,24> light{};
    std::array<std::uint32_t,128> fog{};
    std::array<std::array<std::uint32_t,256>,5> proc{};
    std::array<std::uint32_t,24> ports{};
    std::array<bool,30> dirty{};
    std::array<bool,30> uploaded{};
    std::array<std::uint32_t,30> gpu_first{};
    explicit Model() { dirty.fill(true); }
    Decision Write(unsigned family, unsigned port, std::uint32_t value, unsigned mask, bool enabled) {
        // Backing port mask is deliberately independent of raw special payload.
        std::uint32_t bytes=0;
        for(unsigned i=0;i<4;++i)if(mask&(1u<<i))bytes|=0xffu<<(8*i);
        auto& backing=ports[family*8+port];backing=(backing&~bytes)|(value&bytes);
        Decision d=InvalidTarget();unsigned slot=0;
        if(family==0) {
            slot=lighting.lut_config.type;
            d=StoreRaw(light[slot][lighting.lut_config.index],value,enabled,true);
            lighting.lut_config.index.Assign(lighting.lut_config.index+1);
        } else if(family==1) {
            slot=24;
            d=StoreRaw(fog[texturing.fog_lut_offset%128],value,enabled,true);
            texturing.fog_lut_offset.Assign(texturing.fog_lut_offset+1);
        } else {
            const unsigned selector=static_cast<unsigned>(texturing.proctex_lut_config.ref_table.Value());
            unsigned table=5;
            switch(selector){case 0:table=0;break;case 2:table=1;break;case 3:table=2;break;
                case 4:table=3;break;case 5:table=4;break;}
            slot=25+table;
            if(table<5)d=StoreRaw(proc[table][texturing.proctex_lut_config.index%(table<3?128:256)],value,enabled,true);
            texturing.proctex_lut_config.index.Assign(texturing.proctex_lut_config.index+1);
        }
        if(d.notify && slot<dirty.size())dirty[slot]=true;
        return d;
    }
    void Draw() {
        // Integer shadow upload model only; actual GL readback belongs to oracle.
        for(unsigned i=0;i<dirty.size();++i)if(dirty[i]) {
            gpu_first[i]=i<24?light[i][0]:i==24?fog[0]:proc[i-25][0];
            uploaded[i]=true;dirty[i]=false;
        }
    }
};
int main() {
    // Independent StoreRaw answers, including raw reserved bits and signed zeros.
    for(bool enabled:{false,true})for(bool observe:{false,true}) {
        std::uint32_t raw=0x80000000;
        auto d=StoreRaw(raw,0x80000000,enabled,observe);
        Require(raw==0x80000000 && !d.changed && d.notify==!enabled,"equal target decision");
        d=StoreRaw(raw,0x80000001,enabled,observe);
        Require(raw==0x80000001 && d.changed==(enabled||observe) && d.notify,"raw bit change");
    }
    const auto invalid=InvalidTarget();
    Require(!invalid.valid_target && !invalid.changed && invalid.notify,"invalid target preserves notification");
    Model off,on;
    // Existing dirty initialization cannot be cleared by an unchanged zero write.
    auto d=on.Write(0,0,0,0,true);
    Require(!d.notify && on.dirty[0] && on.lighting.lut_config.index==1,"initial zero retains dirty and advances");
    off.Write(0,0,0,0,false);off.Draw();on.Draw();
    Require(off.uploaded==on.uploaded && off.gpu_first==on.gpu_first,"initial all-zero upload model");
    // Same port payload can change the next actual target; port equality cannot
    // suppress it. Conversely a target repeat may differ from old port contents.
    on.lighting.lut_config.index.Assign(17);on.light[0][17]=0;
    d=on.Write(0,0,0x12345678,15,true);Require(d.changed && d.notify,"actual target changed");
    on.lighting.lut_config.index.Assign(18);on.light[0][18]=0;
    d=on.Write(0,0,0x12345678,15,true);Require(d.changed && d.notify,"same port different target");
    on.lighting.lut_config.index.Assign(17);
    d=on.Write(0,1,0x12345678,0,true);Require(!d.changed && !d.notify,"different port equal target and zero mask");
    // Fresh paired models: all ports/masks, type/table changes and real bitfield
    // wraps. Store every raw payload, including under zero parameter masks.
    off=Model{};on=Model{};
    for(unsigned family=0;family<3;++family)for(unsigned selector=0;selector<(family==0?24u:16u);++selector)
    for(unsigned mask=0;mask<16;++mask)for(unsigned port=0;port<8;++port) {
        off.lighting.lut_config.type.Assign(selector);on.lighting.lut_config.type.Assign(selector);
        off.lighting.lut_config.index.Assign(255);on.lighting.lut_config.index.Assign(255);
        off.texturing.fog_lut_offset.Assign(65535);on.texturing.fog_lut_offset.Assign(65535);
        off.texturing.proctex_lut_config.ref_table.Assign(static_cast<Pica::TexturingRegs::ProcTexLutTable>(selector));
        on.texturing.proctex_lut_config.ref_table.Assign(static_cast<Pica::TexturingRegs::ProcTexLutTable>(selector));
        off.texturing.proctex_lut_config.index.Assign(255);on.texturing.proctex_lut_config.index.Assign(255);
        for(std::uint32_t value:{0u,0x80000000u,0x7fc12345u,0xffffffffu,0u,0u}) {
            const auto a=off.Write(family,port,value,mask,false),b=on.Write(family,port,value,mask,true);
            Require(a.changed==b.changed && b.notify==(!b.valid_target||b.changed),"change/notification decision");
            Require(off.light==on.light && off.fog==on.fog && off.proc==on.proc && off.ports==on.ports,"all guest table/backing words");
            Require(off.lighting.lut_config.index==on.lighting.lut_config.index &&
                off.texturing.fog_lut_offset==on.texturing.fog_lut_offset &&
                off.texturing.proctex_lut_config.index==on.texturing.proctex_lut_config.index,"all auto-indices");
            off.Draw();on.Draw();Require(off.gpu_first==on.gpu_first,"dirty upload shadow model");
        }
        Require(on.lighting.lut_config.index==5 || family!=0,"lighting 8-bit wrap");
        Require(on.texturing.fog_lut_offset==5 || family!=1,"fog 16-bit wrap");
        Require(on.texturing.proctex_lut_config.index==5 || family!=2,"proctex 8-bit wrap including invalid");
    }
    on.dirty[0]=true;on.lighting.lut_config.type.Assign(0);on.lighting.lut_config.index.Assign(0);
    const auto saved=on.light[0][0];on.Write(0,0,saved,15,true);
    Require(on.dirty[0],"unchanged write never clears preexisting dirty");
    auto before=ReadCounters();Record(Kind::Lighting,{false,false,true});Record(Kind::Lighting,{true,true,true});
    Record(Kind::ProcTex,InvalidTarget());auto after=ReadCounters();
    Require(after[0].writes-before[0].writes==2 && after[0].changed-before[0].changed==1 &&
        after[0].notifications-before[0].notifications==1,"actual-change counters");
    Require(after[2].invalid_targets-before[2].invalid_targets==1 &&
        after[2].notifications-before[2].notifications==1,"invalid counters");
    std::cout<<checks<<" LUT port/index/dirty-model checks passed; no GL claim\n";
}
