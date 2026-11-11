// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <iostream>
#include "audio_core/raw_audio_buffer.h"
int main() {
    AudioCore::RawAudioBuffer stream;
    std::array<s16,1024> samples;
    if(stream.Ready(2047,512) || !stream.Ready(2048,512)) return 1;
    samples.fill(16000); stream.Finish(samples.data(),512,512);
    if(samples[0]!=0 || samples[126]!=16000 || samples.back()!=16000) return 2;
    // Continuous playback must leave real samples untouched after the initial ramp.
    samples.fill(-12000); stream.Finish(samples.data(),512,512);
    if(!std::all_of(samples.begin(),samples.end(),[](s16 x){return x==-12000;})) return 3;
    // An entirely missing callback fades from the last frame instead of jumping to silence.
    stream.Finish(samples.data(),0,512);
    if(samples[0]!=-12000 || samples[126]!=0 || samples.back()!=0 || stream.Ready(512,512)) return 4;
    if(!stream.Ready(2048,512)) return 5;
    samples.fill(8000);stream.Finish(samples.data(),512,512);
    if(samples[0]!=0 || samples[126]!=8000 || samples.back()!=8000) return 6;
    // A partial source block ramps its last available samples down and zeroes the remainder.
    samples.fill(10000);stream.Finish(samples.data(),128,512);
    if(samples[126]!=10000 || samples[254]!=0 || samples[256]!=0 || samples.back()!=0) return 7;
    // Small and empty callbacks must not access samples outside the caller's buffer.
    std::array<s16,4> guarded{2222,7777,7777,3333};
    stream.Finish(guarded.data()+1,1,1);
    stream.Finish(guarded.data()+1,0,0);
    if(guarded.front()!=2222 || guarded.back()!=3333) return 8;
    stream.Reset();if(stream.Ready(2048,4096) || !stream.Ready(4096,4096)) return 9;
    std::cout << "raw audio prebuffer, continuous PCM, underrun fades and recovery passed\n";
}
