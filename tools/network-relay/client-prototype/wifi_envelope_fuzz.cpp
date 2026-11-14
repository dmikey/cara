// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#include "wifi_envelope.h"
extern "C" int LLVMFuzzerTestOneInput(const unsigned char* data, std::size_t size) {
    if(size==0) return 0;
    Cara::Relay::Bytes b(data,data+size);
    if(auto p=Cara::Relay::Decode(b)) {
        auto encoded=Cara::Relay::Encode(*p);
        if(!encoded || *encoded!=b) __builtin_trap();
        Cara::Relay::ShouldDeliver(*p,{2,1,2,3,4,5},6);
    }
    if(auto native=Cara::Relay::WireNodeMapToNative(b)) {
        if(Cara::Relay::NativeNodeMapToWire(*native)!=std::optional<Cara::Relay::Bytes>(b)) __builtin_trap();
    }
    Cara::Relay::NativeNodeMapToWire(b);
    return 0;
}
