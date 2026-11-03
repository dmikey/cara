// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/skin_layout.h"
#include <cmath>
#include <iostream>
#include "ios/skin_archive.h"

static std::vector<std::uint8_t> Stored(const std::vector<std::string>& names) {
    std::vector<std::uint8_t> out; std::vector<size_t> offsets;
    const auto u16=[&](unsigned n) { out.push_back(n&255); out.push_back((n>>8)&255); };
    const auto u32=[&](unsigned n) { u16(n&65535); u16(n>>16); };
    for (const auto& name:names) {
        offsets.push_back(out.size()); u32(0x04034b50); u16(20); u16(0); u16(0); u16(0); u16(0);
        u32(0); u32(0); u32(0); u16(name.size()); u16(0); out.insert(out.end(),name.begin(),name.end());
    }
    const size_t directory=out.size();
    for (size_t i=0;i<names.size();++i) {
        const auto& name=names[i]; u32(0x02014b50); u16(20); u16(20); u16(0); u16(0); u16(0); u16(0);
        u32(0); u32(0); u32(0); u16(name.size()); u16(0); u16(0); u16(0); u16(0); u32(0); u32(offsets[i]);
        out.insert(out.end(),name.begin(),name.end());
    }
    const size_t bytes=out.size()-directory;
    u32(0x06054b50); u16(0); u16(0); u16(names.size()); u16(names.size()); u32(bytes); u32(directory); u16(0);
    return out;
}
int main(int argc,char** argv) { @autoreleasepool {
    const auto require=[](bool ok,const char* message) { if (!ok) { std::cerr<<message<<'\n'; std::exit(1); } };
    const auto reject=[&](const std::vector<std::uint8_t>& bytes) { bool bad=false; try { Cara::ReadSkinArchive(bytes); } catch (...) { bad=true; } require(bad,"Malformed archive accepted"); };
    require(Cara::ReadSkinArchive(Stored({"info.json"})).count("info.json"),"Stored ZIP failed");
    reject(Stored({"../info.json"})); reject(Stored({"info.json","info.json"})); reject({1,2,3});
    auto invalid=Stored({"info.json"}); invalid.resize(invalid.size()-1); reject(invalid);
    invalid=Stored({"info.json"}); invalid[18]=255; reject(invalid);
    NSError* error=nil;
    NSString* path=argc>1?[NSString stringWithUTF8String:argv[1]]:@CARA_BUILTIN_SKIN;
    CaraSkin* skin=[CaraSkin loadURL:[NSURL fileURLWithPath:path] error:&error];
    require(skin!=nil,error.localizedDescription.UTF8String?:"Skin load failed");
    NSDictionary* rep=[skin representationForPad:YES landscape:YES];
    require(CaraValidateRepresentation(rep),"Golden representation failed");
    NSMutableDictionary* bad=[rep mutableCopy]; bad[@"mappingSize"]=@{@"width":@0,@"height":@100};
    require(!CaraValidateRepresentation(bad),"Zero mapping accepted");
    bad=[rep mutableCopy]; bad[@"items"]=@[@{@"inputs":@[@"quickSave"],@"frame":@{@"x":@0,@"y":@0,@"width":@40,@"height":@40}}];
    require(!CaraValidateRepresentation(bad),"Unsupported button accepted");
    CGRect fit=CaraAspectFit(CGSizeMake(400,240),CGRectMake(0,0,1024,768));
    require(std::abs(fit.size.height-614.4)<.001 && std::abs(fit.origin.y-76.8)<.001,"3DS upper aspect fit failed");
    CGRect screen=CGRectMake(100,200,640,480);
    CGPoint p=CaraTouchPoint(CGPointMake(420,440),screen);
    require(p.x==160 && p.y==120,"Touch center mapping failed");
    p=CaraTouchPoint(CGPointMake(739.99,679.99),screen);
    require(p.x==319 && p.y==239,"Touch edge clamp failed");
    require(CaraTouchPoint(CGPointMake(99,400),screen).x<0,"Letterbox touch accepted");
    CGRect stick=CGRectMake(0,0,200,200);
    require(CGPointEqualToPoint(CaraStickPoint(CGPointMake(100,100),stick),CGPointZero),"Stick neutral failed");
    p=CaraStickPoint(CGPointMake(200,100),stick); require(p.x==1 && p.y==0,"Stick right failed");
    p=CaraStickPoint(CGPointMake(300,-100),stick); require(std::abs(std::hypot(p.x,p.y)-1)<.001 && p.y>0,"Stick clamp/sign failed");
    require(CaraDPadDirections(CGPointMake(10,10),CGRectMake(0,0,300,300))==5,"D-pad diagonal failed");
    require(CaraDPadDirections(CGPointMake(150,150),CGRectMake(0,0,300,300))==0,"D-pad neutral failed");
    std::cout<<"Skin archive, bounds, unsupported inputs, dual-screen scaling, touch, D-pad and stick checks passed: "<<[skin.info[@"name"] UTF8String]<<'\n';
    return 0;
} }
