// SPDX-License-Identifier: GPL-2.0-or-later
#include "cara/android_dual_display_plan.h"
#include <cassert>
#include <limits>
using namespace Cara::AndroidDualDisplay;
int main() {
    assert(Decode(-1)==Mode::Off && Decode(0)==Mode::Off && Decode(3)==Mode::Off);
    assert(Decode(1)==Mode::TopExternal && Decode(2)==Mode::BottomExternal);
    auto t=Fit(1920,1080,false); assert(t.left==60 && t.right==1860 && t.top==0 && t.bottom==1080);
    auto b=Fit(1920,1080,true); assert(b.left==240 && b.right==1680 && b.top==0 && b.bottom==1080);
    auto p=Fit(1080,1920,true,{10,20,10,20}); assert(p.left==10&&p.right==1070&&p.top==562&&p.bottom==1357);
    assert(!Touch(b,1920,1080,0.f,.5f,false));
    auto center=Touch(b,1920,1080,.5f,.5f,false); assert(center&&center->first==.5f&&center->second==.5f);
    auto clipped=Touch(b,1920,1080,2.f,-1.f,true); assert(clipped&&clipped->first<1&&clipped->first>.999f&&clipped->second==0);
    assert(!Touch(b,1920,1080,std::numeric_limits<float>::quiet_NaN(),0,true));
    assert(!Touch(Fit(1,1,false),1,1,.5f,.5f,false));
    assert(Fit(100,100,true,{100,0,1,0}).right==0);
    assert(Fit(100,100,true,{50,0,51,0}).right==0);
}
