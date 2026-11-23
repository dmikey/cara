// SPDX-License-Identifier: GPL-2.0-or-later
// Include the actual private helper to test unusual device answers and lifetime
// without introducing a public renderer API or changing its serialized headers.
#include "video_core/renderer_metal/pica_metal_upload.mm"
#import <Foundation/Foundation.h>
#include <iostream>
#include <stdexcept>

@interface AlignmentAnswerDevice : NSObject {
@public
    std::array<NSUInteger,3> answers;
    std::array<unsigned,3> queries;
    bool throw_next;
}
@end
@implementation AlignmentAnswerDevice
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format {
    const unsigned slot=format==MTLPixelFormatR16Uint?0:format==MTLPixelFormatRGBA32Uint?1:2;
    ++queries[slot];
    if(throw_next) {
        throw_next=false;
        @throw [NSException exceptionWithName:@"AuthoredAlignmentQueryFailure" reason:@"Retry admission" userInfo:nil];
    }
    return answers[slot];
}
@end
namespace {
void Require(bool value,const char* reason) { if(!value)throw std::runtime_error(reason); }
}
int main() { @autoreleasepool { try {
    const char* flag=std::getenv("CARA_METAL_CACHE_LINEAR_TEXTURE_ALIGNMENT");
    const bool cache=flag && std::string_view(flag)=="1";
    auto first=[AlignmentAnswerDevice new];first->answers={0,3,std::numeric_limits<NSUInteger>::max()};
    const MTLPixelFormat formats[]{MTLPixelFormatR16Uint,MTLPixelFormatRGBA32Uint,MTLPixelFormatRGBA32Float};
    for(unsigned repeat=0;repeat<4;++repeat)for(unsigned slot=0;slot<3;++slot)
        Require(Cara::LinearTextureAlignment((id<MTLDevice>)first,formats[slot])==first->answers[slot],
                "Cached capability answer changed its exact value");
    for(auto count:first->queries)Require(count==(cache?1u:4u),"Format answer query count differs");
    Require(!Cara::Align(8,0) && !Cara::Align(8,3) && !Cara::Align(8,first->answers[2]),
            "Unusual capability answer bypassed alignment guards");
    Require(!Cara::Align(std::numeric_limits<NSUInteger>::max(),256),"Alignment overflow guard changed");
    Require(Cara::Align(257,256)==512,"Valid device alignment changed");
    __weak AlignmentAnswerDevice* retained=first;
    first=nil;
    Require((retained!=nil)==cache,"Cache did not retain exact device identity");
    auto second=[AlignmentAnswerDevice new];second->answers={16,32,64};second->throw_next=true;
    bool rejected=false;
    @try { (void)Cara::LinearTextureAlignment((id<MTLDevice>)second,formats[0]); }
    @catch(NSException*) { rejected=true; }
    Require(rejected,"Device query failure was hidden");
    Require(Cara::LinearTextureAlignment((id<MTLDevice>)second,formats[0])==16,
            "Failed query admitted a stale cache answer");
    Require(Cara::LinearTextureAlignment((id<MTLDevice>)second,formats[0])==16,
            "Retried capability answer changed");
    Require(second->queries[0]==(cache?2u:3u),"Failed query was cached instead of retried");
    Require(retained==nil,"Old device remained retained after replacement");
    for(unsigned slot=1;slot<3;++slot)
        Require(Cara::LinearTextureAlignment((id<MTLDevice>)second,formats[slot])==second->answers[slot],
                "New device inherited a prior device's format answer");
    std::cout<<"PASS: cache="<<cache<<"; exact unusual/zero answers, overflow, independent formats, query failure and retained-device lifetime\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; } } }
