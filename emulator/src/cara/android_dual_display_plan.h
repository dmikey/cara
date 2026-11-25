// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace Cara::AndroidDualDisplay {
enum class Mode : int { Off=0, TopExternal=1, BottomExternal=2 };
constexpr Mode Decode(int value) {
    return value==1?Mode::TopExternal:value==2?Mode::BottomExternal:Mode::Off;
}
struct Rect { unsigned left{},top{},right{},bottom{}; };
struct Insets { unsigned left{},top{},right{},bottom{}; };
constexpr Rect Fit(unsigned width,unsigned height,bool bottom,Insets insets={}) {
    if(!width||!height||insets.left>=width||insets.right>=width-insets.left||
       insets.top>=height||insets.bottom>=height-insets.top)return {};
    const unsigned area_w=width-insets.left-insets.right,area_h=height-insets.top-insets.bottom;
    const unsigned native_w=bottom?320:400,native_h=240;
    unsigned w,h;
    if(std::uint64_t(area_w)*native_h<=std::uint64_t(area_h)*native_w){
        w=area_w;h=static_cast<unsigned>(std::uint64_t(w)*native_h/native_w);
    }else{
        h=area_h;w=static_cast<unsigned>(std::uint64_t(h)*native_w/native_h);
    }
    if(!w||!h)return {};
    const unsigned x=insets.left+(area_w-w)/2,y=insets.top+(area_h-h)/2;
    return {x,y,x+w,y+h};
}
inline std::optional<std::pair<float,float>> Touch(Rect rect,unsigned width,unsigned height,
                                                  float x,float y,bool clip) {
    if(!width||!height||rect.right<=rect.left||rect.bottom<=rect.top||
       rect.right>width||rect.bottom>height||!std::isfinite(x)||!std::isfinite(y))return std::nullopt;
    // Touch coordinates refer to the actual fitted SurfaceView, not its bars.
    const double px=double(x)*width,py=double(y)*height;
    if(!clip&&(px<rect.left||px>=rect.right||py<rect.top||py>=rect.bottom))return std::nullopt;
    const float last=std::nextafter(1.0f,0.0f);
    return std::pair{std::clamp(float((px-rect.left)/(rect.right-rect.left)),0.0f,last),
                     std::clamp(float((py-rect.top)/(rect.bottom-rect.top)),0.0f,last)};
}
}
