// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/skin_layout.h"
#include <cmath>
#include "ios/skin_archive.h"

CGRect CaraSkinRect(NSDictionary* value) {
    if (![value isKindOfClass:NSDictionary.class]) return CGRectNull;
    for (NSString* key in @[@"x",@"y",@"width",@"height"])
        if (![value[key] isKindOfClass:NSNumber.class] || !std::isfinite([value[key] doubleValue])) return CGRectNull;
    return CGRectMake([value[@"x"] doubleValue],[value[@"y"] doubleValue],
                      [value[@"width"] doubleValue],[value[@"height"] doubleValue]);
}
CGRect CaraAspectFit(CGSize source, CGRect destination) {
    const double scale=MIN(destination.size.width/source.width,destination.size.height/source.height);
    return CGRectMake(CGRectGetMidX(destination)-source.width*scale/2,CGRectGetMidY(destination)-source.height*scale/2,source.width*scale,source.height*scale);
}
CGPoint CaraStickPoint(CGPoint p,CGRect r) {
    if (r.size.width<=0 || r.size.height<=0) return CGPointZero;
    const double x=(p.x-CGRectGetMidX(r))/(r.size.width/2),y=(CGRectGetMidY(r)-p.y)/(r.size.height/2);
    const double length=std::hypot(x,y);
    if (length<.12) return CGPointZero;
    const double magnitude=MIN(1.0,(length-.12)/.88);
    return CGPointMake(x/length*magnitude,y/length*magnitude);
}
CGPoint CaraTouchPoint(CGPoint p,CGRect r) {
    if (CGRectIsEmpty(r) || !CGRectContainsPoint(r,p)) return CGPointMake(-1,-1);
    return CGPointMake(MIN(319,MAX(0,(p.x-r.origin.x)/r.size.width*320)),MIN(239,MAX(0,(p.y-r.origin.y)/r.size.height*240)));
}
unsigned CaraDPadDirections(CGPoint p,CGRect r) {
    unsigned value=0;
    if (p.y<r.origin.y+r.size.height/3) value|=1;
    if (p.y>r.origin.y+r.size.height*2/3) value|=2;
    if (p.x<r.origin.x+r.size.width/3) value|=4;
    if (p.x>r.origin.x+r.size.width*2/3) value|=8;
    return value;
}
BOOL CaraValidateRepresentation(NSDictionary* rep) {
    if (![rep isKindOfClass:NSDictionary.class]) return NO;
    NSDictionary* size=rep[@"mappingSize"];
    if (![size isKindOfClass:NSDictionary.class] || ![size[@"width"] isKindOfClass:NSNumber.class] || ![size[@"height"] isKindOfClass:NSNumber.class]) return NO;
    const double w=[size[@"width"] doubleValue],h=[size[@"height"] doubleValue];
    if (!std::isfinite(w)||!std::isfinite(h)||w<100||h<100||w>8192||h>8192||w*h>16777216) return NO;
    if (![rep[@"screens"] isKindOfClass:NSArray.class] || [rep[@"screens"] count]!=2 ||
        ![rep[@"items"] isKindOfClass:NSArray.class] || [rep[@"items"] count]>64 ||
        ![rep[@"assets"] isKindOfClass:NSDictionary.class]) return NO;
    CGRect bounds=CGRectMake(0,0,w,h);
    for (NSDictionary* screen in rep[@"screens"]) {
        if (![screen isKindOfClass:NSDictionary.class]) return NO;
        CGRect r=CaraSkinRect(screen[@"outputFrame"]);
        if (CGRectIsNull(r)||CGRectIsEmpty(r)||!CGRectContainsRect(bounds,r)) return NO;
    }
    NSSet* buttons=[NSSet setWithArray:@[@"a",@"b",@"x",@"y",@"up",@"down",@"left",@"right",@"l",@"r",@"zl",@"zr",@"start",@"select",@"menu"]];
    NSSet* axes=[NSSet setWithArray:@[@"touchScreenX",@"touchScreenY",@"circlePadX",@"circlePadY",@"cStickX",@"cStickY",@"up",@"down",@"left",@"right"]];
    for (NSDictionary* item in rep[@"items"]) {
        if (![item isKindOfClass:NSDictionary.class]) return NO;
        CGRect r=CaraSkinRect(item[@"frame"]);
        if (CGRectIsNull(r)||CGRectIsEmpty(r)||!CGRectContainsRect(bounds,r)) return NO;
        id input=item[@"inputs"];
        if ([input isKindOfClass:NSArray.class]) {
            if (![input count]) return NO;
            for (id name in input) if (![buttons containsObject:name]) return NO;
        } else if ([input isKindOfClass:NSDictionary.class]) {
            if (![input count]) return NO;
            for (id name in [input allValues]) if (![axes containsObject:name]) return NO;
            if (input[@"x"] || input[@"y"]) {
                if ([input count]!=2 || !input[@"x"] || !input[@"y"]) return NO;
                BOOL pair=([input[@"x"] isEqual:@"touchScreenX"] && [input[@"y"] isEqual:@"touchScreenY"]) ||
                          ([input[@"x"] isEqual:@"circlePadX"] && [input[@"y"] isEqual:@"circlePadY"]) ||
                          ([input[@"x"] isEqual:@"cStickX"] && [input[@"y"] isEqual:@"cStickY"]);
                if (!pair) return NO;
            } else {
                if ([input count]!=4) return NO;
                for (NSString* key in @[@"up",@"down",@"left",@"right"]) if (![buttons containsObject:input[key]]) return NO;
            }
        } else return NO;
        NSDictionary* edges=item[@"extendedEdges"];
        if (edges) {
            if (![edges isKindOfClass:NSDictionary.class]) return NO;
            for (NSString* k in edges) if (![edges[k] isKindOfClass:NSNumber.class] ||
                !std::isfinite([edges[k] doubleValue])||[edges[k] doubleValue]<0||[edges[k] doubleValue]>512) return NO;
        }
        if (item[@"thumbstick"]) {
            NSDictionary* thumb=item[@"thumbstick"];
            if (![thumb isKindOfClass:NSDictionary.class] || ![thumb[@"name"] isKindOfClass:NSString.class]) return NO;
            for (NSString* key in @[@"width",@"height"])
                if (![thumb[key] isKindOfClass:NSNumber.class] || !std::isfinite([thumb[key] doubleValue]) || [thumb[key] doubleValue]<=0 || [thumb[key] doubleValue]>8192) return NO;
        }
    }
    return YES;
}
@implementation CaraSkin
+ (instancetype)loadURL:(NSURL*)url error:(NSError**)error {
    NSString* problem=@"This is not a supported dual-screen Delta/Cara skin.";
    @try {
        if (!url.isFileURL) @throw [NSException exceptionWithName:@"CaraSkin" reason:@"Expected a local skin file" userInfo:nil];
        NSData* data=[NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error];
        if (!data) return nil;
        try {
            const auto* bytes=static_cast<const std::uint8_t*>(data.bytes);
            auto files=Cara::ReadSkinArchive(std::vector<std::uint8_t>(bytes,bytes+data.length));
            NSMutableDictionary* assets=[NSMutableDictionary new];
            for (const auto& [name,value]:files) {
                NSString* key=[[NSString alloc] initWithBytes:name.data() length:name.size() encoding:NSUTF8StringEncoding];
                if (!key) throw std::runtime_error("Invalid filename");
                assets[key]=[NSData dataWithBytes:value.data() length:value.size()];
            }
            NSDictionary* info=[NSJSONSerialization JSONObjectWithData:assets[@"info.json"] options:0 error:nil];
            if (![info isKindOfClass:NSDictionary.class] || ![info[@"name"] isKindOfClass:NSString.class] ||
                ![info[@"representations"] isKindOfClass:NSDictionary.class] ||
                ![info[@"gameTypeIdentifier"] isKindOfClass:NSString.class]) throw std::runtime_error("Invalid metadata");
            if ([info[@"name"] length]>128 || [info[@"gameTypeIdentifier"] length]>128) throw std::runtime_error("Oversized metadata");
            NSString* type=[info[@"gameTypeIdentifier"] lowercaseString];
            if (![type hasSuffix:@".ds"] && ![type hasSuffix:@".3ds"]) throw std::runtime_error("Wrong system");
            CaraSkin* skin=[CaraSkin new]; skin->_info=[info copy]; skin->_assets=[assets copy];
            BOOL found=NO;
            for (NSString* device in info[@"representations"]) {
                id styles=info[@"representations"][device];
                if (![styles isKindOfClass:NSDictionary.class]) throw std::runtime_error("Invalid representation");
                for (NSString* style in styles) {
                    id orientations=styles[style];
                    if (![orientations isKindOfClass:NSDictionary.class]) throw std::runtime_error("Invalid orientation");
                    for (NSString* orientation in orientations) {
                        NSDictionary* rep=orientations[orientation];
                        if (!CaraValidateRepresentation(rep)) throw std::runtime_error("Unsupported controls or layout");
                        NSDictionary* images=rep[@"assets"];
                        NSString* asset=images[@"resizable"]?:images[@"large"]?:images[@"medium"]?:images[@"small"];
                        if (![asset isKindOfClass:NSString.class] || !assets[asset]) throw std::runtime_error("Missing artwork");
                        found=YES;
                    }
                }
            }
            if (found) return skin;
        } catch (const std::exception& e) {
            problem=[NSString stringWithFormat:@"Could not load skin: %s",e.what()];
        }
    } @catch (NSException* exception) { problem=@"The skin contains invalid metadata."; }
    if (error) *error=[NSError errorWithDomain:@"CaraSkin" code:1 userInfo:@{NSLocalizedDescriptionKey:problem}];
    return nil;
}
- (NSDictionary*)representationForPad:(BOOL)pad landscape:(BOOL)landscape {
    NSDictionary* devices=self.info[@"representations"];
    for (NSString* device in pad?@[@"ipad",@"iphone"]:@[@"iphone",@"ipad"]) {
        for (NSString* style in @[@"standard",@"edgeToEdge",@"splitView"]) {
            NSDictionary* orientations=devices[device][style];
            NSDictionary* rep=orientations[landscape?@"landscape":@"portrait"]?:orientations[@"landscape"]?:orientations[@"portrait"];
            if (rep) return rep;
        }
    }
    return nil;
}
@end
