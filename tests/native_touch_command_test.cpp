// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#include <cassert>
#include <iostream>
#include "cara/native_mac/touch_command.h"
int main() {
    using namespace NativeMac;
    const auto first=ParseTouchCommand("touch 0 0 1");assert(first && first->frames==1);
    const auto last=ParseTouchCommand("touch 319 239 600");assert(last && last->frames==600);
    for (const auto* line : {"touch -1 0 1","touch 320 0 1","touch 0 -1 1","touch 0 240 1",
         "touch 0 0 0","touch 0 0 601","touch 0 0 1 extra","touch 0 0","touch 1.5 0 1",
         "touch 999999999999999999999 0 1","a 0 0 1",""}) assert(!ParseTouchCommand(line));
    assert(!ParseTouchCommand(std::string(96,' ')));
    auto a=TouchFramebufferPoint(*first,40,480,680,960,true);assert(a && a->first==41 && a->second==481);
    auto b=TouchFramebufferPoint(*last,40,480,680,960,true);assert(b && b->first==679 && b->second==959);
    for(unsigned x=0;x<320;++x)for(unsigned y=0;y<240;++y){
        const auto p=TouchFramebufferPoint({x,y,6},40,480,680,960,true);assert(p);
        assert((p->first-40)*320/640==x && (p->second-480)*240/480==y);
    }
    assert(!TouchFramebufferPoint(*first,0,0,0,240,true));
    assert(!TouchFramebufferPoint(*first,0,0,320,240,false));
    std::cout << "PASS native diagnostic touch bounds and 76800 guest coordinate mappings\n";
}
