// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version. Refer to license.txt.
#include "audio_core/hle/apple_aac_codec.h"
#include "apple_aac_fixture.h"
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>
using namespace AudioCore::HLE;
#define CHECK(x) do { if (!(x)) { std::cerr << "AAC check failed line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)
static void Append(AppleAACResult& all, const AppleAACResult& next) {
    all.sample_rate=next.sample_rate; all.channels=next.channels;
    for(unsigned c=0;c<2;++c) all.pcm[c].insert(all.pcm[c].end(),next.pcm[c].begin(),next.pcm[c].end());
}
// Independent container/framing oracle through Apple's file decoder, same codec math backend.
static AppleAACResult Reference(const std::uint8_t* bytes,std::size_t size,unsigned rate,unsigned channels) {
    char path[]="/tmp/cara-original-aac-XXXXXX";
    int fd=mkstemp(path); CHECK(fd>=0);
    CHECK(write(fd,bytes,size)==static_cast<ssize_t>(size)); close(fd);
    auto url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<UInt8*>(path),std::strlen(path),false);
    ExtAudioFileRef file=nullptr; auto status=ExtAudioFileOpenURL(url,&file); CFRelease(url); unlink(path); CHECK(status==noErr);
    AudioStreamBasicDescription format{}; format.mSampleRate=rate;format.mFormatID=kAudioFormatLinearPCM;
    format.mFormatFlags=kLinearPCMFormatFlagIsSignedInteger|kLinearPCMFormatFlagIsPacked;
    format.mBytesPerFrame=format.mBytesPerPacket=2*channels;format.mFramesPerPacket=1;
    format.mChannelsPerFrame=channels;format.mBitsPerChannel=16;
    CHECK(ExtAudioFileSetProperty(file,kExtAudioFileProperty_ClientDataFormat,sizeof(format),&format)==noErr);
    AppleAACResult result;result.sample_rate=rate;result.channels=channels;
    std::int16_t pcm[8192];
    for(;;){UInt32 n=4096;AudioBufferList buffer{1,{{channels,static_cast<UInt32>(sizeof(pcm)),pcm}}};
        CHECK(ExtAudioFileRead(file,&n,&buffer)==noErr); if(!n)break;
        for(unsigned i=0;i<n;++i)for(unsigned c=0;c<channels;++c)result.pcm[c].push_back(pcm[i*channels+c]);}
    ExtAudioFileDispose(file);return result;
}
static double Tone(const std::vector<std::int16_t>& pcm,unsigned rate,unsigned frequency){
    double real=0,imag=0;for(std::size_t i=0;i<pcm.size();++i){double phase=2*3.141592653589793*frequency*i/rate;real+=pcm[i]*std::cos(phase);imag+=pcm[i]*std::sin(phase);}return std::hypot(real,imag);
}
static void Test(const std::uint8_t* bytes,std::size_t size,unsigned rate,unsigned channels){
    AppleAACCodec codec;AppleAACResult full;
    CHECK(codec.Decode(bytes,size,full));CHECK(full.sample_rate==rate);CHECK(full.channels==channels);
    CHECK(full.pcm[0].size()>2048);CHECK(full.pcm[channels-1].size()==full.pcm[0].size());
    CHECK(full.pcm==Reference(bytes,size,rate,channels).pcm);
    if(channels==2){CHECK(Tone(full.pcm[0],rate,440)>10*Tone(full.pcm[0],rate,880));CHECK(Tone(full.pcm[1],rate,880)>10*Tone(full.pcm[1],rate,440));}
    codec.Reset();AppleAACResult fragmented;
    for(std::size_t i=0;i<size;){std::size_t n=std::min<std::size_t>(13,size-i);AppleAACResult part;CHECK(codec.Decode(bytes+i,n,part));Append(fragmented,part);i+=n;}
    CHECK(fragmented.pcm==full.pcm);
    codec.Reset();AppleAACResult per_packet;
    for(std::size_t i=0;i<size;){std::size_t n=((bytes[i+3]&3)<<11)|(bytes[i+4]<<3)|(bytes[i+5]>>5);
        AppleAACResult part;CHECK(n && i+n<=size);CHECK(codec.Decode(bytes+i,n,part));Append(per_packet,part);i+=n;}
    CHECK(per_packet.pcm==full.pcm);
    std::vector<std::uint8_t> crc;
    for(std::size_t i=0;i<size;){std::size_t n=((bytes[i+3]&3)<<11)|(bytes[i+4]<<3)|(bytes[i+5]>>5);
        auto start=crc.size();crc.insert(crc.end(),bytes+i,bytes+i+7);crc.push_back(0);crc.push_back(0);
        crc.insert(crc.end(),bytes+i+7,bytes+i+n);auto* h=crc.data()+start;h[1]&=~1;
        auto length=n+2;h[3]=(h[3]&252)|(length>>11);h[4]=length>>3;h[5]=(h[5]&31)|(length<<5);i+=n;}
    codec.Reset();AppleAACResult crc_result;CHECK(codec.Decode(crc.data(),crc.size(),crc_result));CHECK(crc_result.pcm==full.pcm);
    codec.Reset();AppleAACResult replay;CHECK(codec.Decode(bytes,size,replay));CHECK(replay.pcm==full.pcm);
    AppleAACCodec restored;AppleAACResult recovery;CHECK(restored.Decode(bytes,size,recovery));CHECK(recovery.pcm==full.pcm);
    std::uint8_t bad[7]={255,241,0,0,0,0,0};CHECK(!codec.Decode(bad,sizeof(bad),replay));CHECK(codec.Decode(bytes,size,replay));CHECK(replay.pcm==full.pcm);
    std::vector<std::uint8_t> unsupported(bytes,bytes+size);
    unsupported[2]=(unsupported[2]&63);CHECK(!codec.Decode(unsupported.data(),unsupported.size(),replay));
    unsupported.assign(bytes,bytes+size);unsupported[6]|=1;CHECK(!codec.Decode(unsupported.data(),unsupported.size(),replay));
    unsupported.assign(bytes,bytes+size);unsupported.insert(unsupported.end(),Mono44,Mono44+sizeof(Mono44));
    if(channels!=1)CHECK(!codec.Decode(unsupported.data(),unsupported.size(),replay));
    CHECK(!codec.Decode(bytes,AppleAACCodec::MaxInput+1,replay));
    codec.Reset();CHECK(codec.Decode(bytes,5,replay));CHECK(replay.pcm[0].empty());codec.Reset();CHECK(codec.Decode(bytes,size,replay));CHECK(replay.pcm==full.pcm);
    std::cout<<"AAC "<<rate<<"Hz channels="<<channels<<" frames="<<full.pcm[0].size()<<" reference/fragment/reset/recovery passed\n";
}
int main(){Test(Stereo32,sizeof(Stereo32),32000,2);Test(Mono44,sizeof(Mono44),44100,1);Test(Stereo48,sizeof(Stereo48),48000,2);}
