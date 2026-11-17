// SPDX-License-Identifier: GPL-2.0-or-later
#include "../emulator/src/cara/texture_copy_trace.h"
#include <cassert>
#include <iostream>
#include <thread>
using namespace Cara::TextureCopyTrace;
struct FakeSurface { unsigned addr=0x1000,size=128,width=8,height=4,stride=8,pixel_format=2;
    bool is_tiled=true; unsigned res_scale=1,type=0; };
int main() {
    Configure(false,"",0,4); VBlank(); { Request request({}); SetReason(Reason::SourceMiss); }
    assert(capture.requests==0 && !Enabled());
    Configure(true,"",2,2); VBlank(); VBlank();
    { Request request({}); } assert(capture.requests==0);
    VBlank(); assert(Enabled());
    {
        Request request({0x1000,0x2000,33,32,16,16,32,0,8});
        Scope lookup(Phase::SourceLookup);
        Source(FakeSurface{}); SetReason(Reason::SourceMiss); current->copied_bytes=32;
        for(unsigned i=0;i<9;++i) DirtySurface(FakeSurface{},0x1000+i*16,16);
        for(unsigned i=0;i<5;++i) DownloadSurface(FakeSurface{},0,0,8,4,128);
        { Scope read(Phase::ReadPixels); }
        std::thread foreign([]{ Request request({}); SetReason(Reason::Accelerated); });
        foreign.join();
    }
    assert(capture.requests==1 && !current);
    assert(capture.records[0].reason==Reason::SourceMiss && capture.records[0].source_surface.valid);
    assert(capture.records[0].dirty_flush_count==9 && capture.records[0].download_count==5);
    assert(capture.records[0].downloads[0].parent_phase==static_cast<unsigned>(Phase::SourceLookup));
    assert(capture.records[0].phases[static_cast<unsigned>(Phase::ReadPixels)].calls==1);
    { Request request({}); assert(current && !current->source_surface.valid); }
    assert(capture.records[1].reason==Reason::Unknown);
    VBlank(); assert(Enabled()); VBlank(); assert(!Enabled() && complete);
    { Request request({}); } assert(capture.requests==2);
    auto bad=Difference({},{}); assert(bad.clock_failures==2 && bad.wall==0 && bad.cpu==0);
    Clocks a{9,10,true,true},b{8,20,true,true}; auto backwards=Difference(a,b);
    assert(backwards.clock_failures==1 && backwards.wall==0 && backwards.cpu==10);
    Configure(true,"",0,4); VBlank();
    for(unsigned i=0;i<260;++i) { Request request({}); current->copied_bytes=16; }
    assert(capture.requests==260 && capture.size==256 && capture.dropped_details==4);
    assert(capture.copied_bytes==260*16 && capture.reasons[0]==260);
    assert(capture.phases[static_cast<unsigned>(Phase::RequestTotal)].calls==260);
    for(unsigned i=0;i<4;++i) VBlank(); assert(complete);
    Write(std::cout);
}
