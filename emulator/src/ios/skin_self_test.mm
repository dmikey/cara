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
    for (BOOL pad : {NO,YES}) for (BOOL landscape : {NO,YES}) {
        NSDictionary* orientation=[skin strictRepresentationForPad:pad landscape:landscape];
        require(CaraValidateRepresentation(orientation),"Built-in portrait/landscape representation missing");
        CGRect stats=CaraSkinRect(orientation[@"caraStatsFrame"]);
        require(stats.size.width>0 && stats.size.height>0,"Stats corner missing");
        CGRect canvas=CGRectMake(0,0,[orientation[@"mappingSize"][@"width"] doubleValue],[orientation[@"mappingSize"][@"height"] doubleValue]);
        CaraHUDLayout hud=CaraSkinHUDLayout(orientation,canvas);
        require(CGRectContainsRect(canvas,hud.indicator) && CGRectContainsRect(canvas,hud.metrics),"HUD outside skin bounds");
        require(CGRectGetMaxX(hud.indicator)<CGRectGetMinX(hud.metrics),"LED must precede metrics without overlapping");
        for (NSDictionary* screen in orientation[@"screens"]) {
            CGRect output=CaraSkinRect(screen[@"outputFrame"]);
            require(!CGRectIntersectsRect(output,hud.indicator) && !CGRectIntersectsRect(output,hud.metrics),"HUD covers a game screen");
        }
        NSMutableDictionary* legacy=[orientation mutableCopy]; [legacy removeObjectForKey:@"caraStatusFrame"];
        CaraHUDLayout fallback=CaraSkinHUDLayout(legacy,canvas);
        require(CGRectContainsRect(stats,fallback.indicator) && CGRectContainsRect(stats,fallback.metrics),"Imported skin HUD footprint changed");
        require([orientation[@"screens"] count]==2,"Both screens required in each orientation");
    }
    // Exercise both orientations of the 390×844-point iPhone 17e with cutout
    // and home-indicator space excluded, plus some extra safe-area margin.
    for (BOOL landscape : {NO,YES}) {
        NSDictionary* phone=[skin strictRepresentationForPad:NO landscape:landscape];
        require(phone!=[skin strictRepresentationForPad:YES landscape:landscape],
                "Phone fell back to the iPad skin");
        CGRect safe=landscape ? CGRectMake(50,0,744,359) : CGRectMake(0,49,390,758);
        CGSize mapping=CGSizeMake([phone[@"mappingSize"][@"width"] doubleValue],
                                  [phone[@"mappingSize"][@"height"] doubleValue]);
        CGRect canvas=CaraAspectFit(mapping,safe);
        require(CGRectContainsRect(CGRectInset(safe,-.001,-.001),canvas),"Phone skin escaped the safe area");
        const CGFloat scale=canvas.size.width/mapping.width;
        NSArray* screens=phone[@"screens"];
        const CGRect upper=CaraSkinRect(screens[0][@"outputFrame"]);
        const CGRect lower=CaraSkinRect(screens[1][@"outputFrame"]);
        require(!CGRectIntersectsRect(upper,lower),"Phone screens overlap");
        require(std::abs(upper.size.width/upper.size.height-400.0/240)<.001 &&
                std::abs(lower.size.width/lower.size.height-320.0/240)<.001,
                "Phone screen aspect ratio changed");
        require(upper.size.width*scale>=290 && lower.size.width*scale>=190,
                "Phone screens too small for the 17e");
        const CGRect message=CaraSkinRect(phone[@"caraMessageFrame"]);
        const CGRect menu=CaraSkinTouchMenuFrame(phone);
        require(!CGRectIsNull(menu),"Phone Menu frame is missing");
        require(!CGRectIsNull(message) && CGRectContainsRect(CGRectMake(0,0,mapping.width,mapping.height),message),
                "Phone status message is missing or outside the canvas");
        require(!CGRectIntersectsRect(message,upper) && !CGRectIntersectsRect(message,lower),
                "Phone status message covers a screen");
        std::vector<CGRect> targets;
        NSMutableSet* buttons=[NSMutableSet new];
        BOOL touchScreen=NO;
        for (NSDictionary* item in phone[@"items"]) {
            const CGRect frame=CaraSkinRect(item[@"frame"]);
            id inputs=item[@"inputs"];
            if ([inputs isKindOfClass:NSDictionary.class] && [inputs[@"x"] isEqual:@"touchScreenX"]) {
                touchScreen=CGRectEqualToRect(frame,lower);
                continue;
            }
            if ([inputs isKindOfClass:NSArray.class]) [buttons addObjectsFromArray:inputs];
            require(frame.size.width*scale>=44 && frame.size.height*scale>=44,
                    "Phone control dropped below 44 points");
            require(!CGRectIntersectsRect(frame,upper) && !CGRectIntersectsRect(frame,lower) &&
                    !CGRectIntersectsRect(frame,message),"Phone control covers a screen or status");
            for (CGRect other : targets)
                require(!CGRectIntersectsRect(frame,other),"Overlapping phone controls");
            targets.push_back(frame);
        }
        NSSet* expected=[NSSet setWithArray:@[@"a",@"b",@"x",@"y",@"l",@"r",@"zl",@"zr",@"start",@"select",@"menu"]];
        require(touchScreen && [buttons isEqualToSet:expected] && targets.size()==14,
                "Phone controls or lower LCD touch mapping are incomplete");
    }
    for (BOOL landscape : {NO,YES}) {
        NSDictionary* controller=CaraControllerSkinRepresentation(landscape);
        require(CaraValidateRepresentation(controller),"Built-in controller skin is invalid");
        require(!CaraSkinHasTouchMenu(controller),"Built-in controller skin should get the shared menu button");
        NSArray* screens=controller[@"screens"];
        CGRect top=CaraSkinRect(screens[0][@"outputFrame"]),bottom=CaraSkinRect(screens[1][@"outputFrame"]);
        require(!CGRectIntersectsRect(top,bottom),"Controller skin screens overlap");
        require(landscape?CGRectGetMinX(bottom)>CGRectGetMaxX(top):CGRectGetMinY(bottom)>CGRectGetMaxY(top),
                "Controller skin screen order");
        NSArray* items=controller[@"items"];
        require(items.count==1 && CGRectEqualToRect(CaraSkinRect(items[0][@"frame"]),bottom) &&
                [items[0][@"inputs"][@"x"] isEqual:@"touchScreenX"],"Controller skin leaves the bottom screen touchable");
        CGRect canvas=CGRectMake(0,0,[controller[@"mappingSize"][@"width"] doubleValue],[controller[@"mappingSize"][@"height"] doubleValue]);
        const CaraHUDLayout hud=CaraSkinHUDLayout(controller,canvas);
        require(!CGRectIntersectsRect(top,hud.metrics) && !CGRectIntersectsRect(bottom,hud.metrics),"Controller HUD covers a game screen");
    }
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
