// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/skin_controls.h"
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>
#include <cmath>

static UIImage* SkinImage(NSData* data, CGSize size) {
    if (!data.length || size.width<=0 || size.height<=0) return nil;
    UIGraphicsImageRendererFormat* format=[UIGraphicsImageRendererFormat defaultFormat]; format.scale=1; format.opaque=NO;
    UIGraphicsImageRenderer* renderer=[[UIGraphicsImageRenderer alloc] initWithSize:size format:format];
    if (data.length>=4 && memcmp(data.bytes,"%PDF",4)==0) {
        CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
        CGPDFDocumentRef document=CGPDFDocumentCreateWithProvider(provider); CGDataProviderRelease(provider);
        if (!document || CGPDFDocumentGetNumberOfPages(document)!=1) { if (document) CGPDFDocumentRelease(document); return nil; }
        CGPDFPageRef page=CGPDFDocumentGetPage(document,1);
        UIImage* image=[renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
            CGContextRef cg=context.CGContext;
            CGContextTranslateCTM(cg,0,size.height); CGContextScaleCTM(cg,1,-1);
            CGContextConcatCTM(cg,CGPDFPageGetDrawingTransform(page,kCGPDFMediaBox,CGRectMake(0,0,size.width,size.height),0,false));
            CGContextDrawPDFPage(cg,page);
        }];
        CGPDFDocumentRelease(document); return image;
    }
    CGImageSourceRef source=CGImageSourceCreateWithData((__bridge CFDataRef)data,nullptr);
    if (!source) return nil;
    NSDictionary* properties=CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source,0,nullptr));
    const double w=[properties[(__bridge NSString*)kCGImagePropertyPixelWidth] doubleValue];
    const double h=[properties[(__bridge NSString*)kCGImagePropertyPixelHeight] doubleValue];
    if (w<=0||h<=0||w>8192||h>8192||w*h>16777216) { CFRelease(source); return nil; }
    CGImageRef cg=CGImageSourceCreateImageAtIndex(source,0,nullptr); CFRelease(source);
    if (!cg) return nil;
    UIImage* original=[UIImage imageWithCGImage:cg]; CGImageRelease(cg);
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext* c) { [original drawInRect:CGRectMake(0,0,size.width,size.height)]; }];
}

@implementation CaraSkinControls {
    NSMutableDictionary<NSValue*,NSDictionary*>* contacts;
    NSMutableDictionary<NSString*,UIImage*>* thumbs;
    BOOL menuHeld;
    NSMutableArray* knobSprites;
    NSMutableArray* highlights;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self=[super initWithFrame:frame])) {
        self.multipleTouchEnabled=YES; self.opaque=NO; self.backgroundColor=UIColor.clearColor;
        contacts=[NSMutableDictionary new]; thumbs=[NSMutableDictionary new]; knobSprites=[NSMutableArray new]; highlights=[NSMutableArray new];
        self.isAccessibilityElement=NO;
    }
    return self;
}
- (void)setRepresentation:(NSDictionary*)representation {
    [self reset]; _representation=representation; [thumbs removeAllObjects];
    for (UIView* view in knobSprites) if ([view isKindOfClass:UIView.class]) [view removeFromSuperview];
    for (CAShapeLayer* layer in highlights) if ([layer isKindOfClass:CAShapeLayer.class]) [layer removeFromSuperlayer];
    [knobSprites removeAllObjects]; [highlights removeAllObjects];
    CGSize size=CGSizeMake([representation[@"mappingSize"][@"width"] doubleValue],[representation[@"mappingSize"][@"height"] doubleValue]);
    NSArray* screens=representation[@"screens"];
    _upperScreen=CaraAspectFit(CGSizeMake(400,240),CaraSkinRect(screens[0][@"outputFrame"]));
    _lowerScreen=CaraAspectFit(CGSizeMake(320,240),CaraSkinRect(screens[1][@"outputFrame"]));
    NSDictionary* assets=representation[@"assets"];
    NSString* name=assets[@"resizable"]?:assets[@"large"]?:assets[@"medium"]?:assets[@"small"];
    UIImage* raw=SkinImage(self.skin.assets[name],size);
    if (!raw) { _artwork=nil; return; }
    UIGraphicsImageRendererFormat* format=[UIGraphicsImageRendererFormat defaultFormat]; format.scale=1; format.opaque=NO;
    _artwork=[[[UIGraphicsImageRenderer alloc] initWithSize:size format:format] imageWithActions:^(UIGraphicsImageRendererContext* context) {
        [raw drawInRect:CGRectMake(0,0,size.width,size.height)];
        CGContextSetBlendMode(context.CGContext,kCGBlendModeClear);
        CGContextFillRect(context.CGContext,self->_upperScreen); CGContextFillRect(context.CGContext,self->_lowerScreen);
    }];
    for (NSDictionary* item in representation[@"items"]) {
        NSDictionary* thumb=item[@"thumbstick"];
        if (thumb) {
            NSString* name=thumb[@"name"]; UIImage* image=thumbs[name];
            if (!image) { image=SkinImage(self.skin.assets[name],CGSizeMake(256,256)); if (image) thumbs[name]=image; }
            UIImageView* sprite=[[UIImageView alloc] initWithImage:image]; sprite.userInteractionEnabled=NO; sprite.opaque=NO;
            if (!image) { sprite.backgroundColor=[UIColor colorWithRed:.3 green:.4 blue:.3 alpha:1]; sprite.layer.cornerRadius=24; }
            [self addSubview:sprite]; [knobSprites addObject:sprite]; [highlights addObject:NSNull.null];
        } else {
            [knobSprites addObject:NSNull.null];
            if ([item[@"inputs"] isKindOfClass:NSDictionary.class] && item[@"inputs"][@"x"]) { [highlights addObject:NSNull.null]; continue; }
            CAShapeLayer* layer=[CAShapeLayer layer]; layer.fillColor=[UIColor colorWithWhite:1 alpha:.18].CGColor;
            layer.strokeColor=[UIColor colorWithRed:.9 green:1 blue:.8 alpha:.8].CGColor; layer.lineWidth=1.5; layer.hidden=YES;
            [self.layer addSublayer:layer]; [highlights addObject:layer];
        }
    }
    [self setNeedsLayout];
}
- (CGPoint)mappingPoint:(CGPoint)point {
    NSDictionary* size=self.representation[@"mappingSize"];
    return CGPointMake(point.x/self.bounds.size.width*[size[@"width"] doubleValue],point.y/self.bounds.size.height*[size[@"height"] doubleValue]);
}
- (NSDictionary*)hitItem:(CGPoint)p {
    for (NSDictionary* item in self.representation[@"items"]) {
        CGRect frame=CaraSkinRect(item[@"frame"]); NSDictionary* edge=item[@"extendedEdges"];
        CGRect hit=CGRectMake(frame.origin.x-[edge[@"left"] doubleValue],frame.origin.y-[edge[@"top"] doubleValue],
            frame.size.width+[edge[@"left"] doubleValue]+[edge[@"right"] doubleValue],
            frame.size.height+[edge[@"top"] doubleValue]+[edge[@"bottom"] doubleValue]);
        if (CGRectContainsPoint(hit,p)) return item;
    }
    return nil;
}
- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent*)event {
    if (!self.representation || self.bounds.size.width<=0 || self.bounds.size.height<=0) return NO;
    return [self hitItem:[self mappingPoint:point]] != nil;
}
- (void)reset { [contacts removeAllObjects]; menuHeld=NO; if (self.inputChanged) self.inputChanged([NSSet set],CGPointZero,CGPointZero,CGPointMake(-1,-1)); [self setNeedsLayout]; }
- (void)updateContact:(NSValue*)key atPoint:(CGPoint)point {
    CGPoint p=[self mappingPoint:point]; NSDictionary* old=contacts[key];
    NSDictionary* item=old?old[@"item"]:[self hitItem:p];
    if (item) contacts[key]=@{@"item":item,@"point":[NSValue valueWithCGPoint:p]};
    [self emit];
}
- (void)endContact:(NSValue*)identifier { [contacts removeObjectForKey:identifier]; [self emit]; }
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) [self updateContact:[NSValue valueWithNonretainedObject:touch] atPoint:[touch locationInView:self]];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) [self updateContact:[NSValue valueWithNonretainedObject:touch] atPoint:[touch locationInView:self]];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) [self endContact:[NSValue valueWithNonretainedObject:touch]];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self touchesEnded:touches withEvent:event]; }
- (CGPoint)stickPoint:(CGPoint)p frame:(CGRect)r {
    return CaraStickPoint(p,r);
}
- (void)emit {
    NSMutableSet* pressed=[NSMutableSet new]; CGPoint circle=CGPointZero,cstick=CGPointZero,touchPoint=CGPointMake(-1,-1);
    BOOL menu=NO;
    for (NSDictionary* contact in contacts.allValues) {
        NSDictionary* item=contact[@"item"]; CGPoint p=[contact[@"point"] CGPointValue]; CGRect r=CaraSkinRect(item[@"frame"]);
        id inputs=item[@"inputs"];
        if ([inputs isKindOfClass:NSArray.class]) {
            // Sliding a finger off a button releases it; a held contact can slide back.
            if ([self hitItem:p] != item && !CGRectContainsPoint(r,p)) continue;
            for (NSString* key in inputs) { if ([key isEqual:@"menu"]) menu=YES; else [pressed addObject:key]; }
        } else if ([inputs[@"x"] isEqual:@"touchScreenX"]) {
            touchPoint=CaraTouchPoint(p,self.lowerScreen);
        } else if ([inputs[@"x"] isEqual:@"circlePadX"]) circle=[self stickPoint:p frame:r];
        else if ([inputs[@"x"] isEqual:@"cStickX"]) cstick=[self stickPoint:p frame:r];
        else {
            const unsigned directions=CaraDPadDirections(p,r);
            if ((directions&1) && inputs[@"up"]) [pressed addObject:inputs[@"up"]];
            if ((directions&2) && inputs[@"down"]) [pressed addObject:inputs[@"down"]];
            if ((directions&4) && inputs[@"left"]) [pressed addObject:inputs[@"left"]];
            if ((directions&8) && inputs[@"right"]) [pressed addObject:inputs[@"right"]];
        }
    }
    if (self.inputChanged) self.inputChanged(pressed,circle,cstick,touchPoint);
    if (menu && !menuHeld && self.menuPressed) dispatch_async(dispatch_get_main_queue(),self.menuPressed);
    menuHeld=menu; [self setNeedsLayout];
}
- (CGRect)menuFrame {
    for (NSDictionary* item in self.representation[@"items"])
        if ([item[@"inputs"] isKindOfClass:NSArray.class] && [item[@"inputs"] containsObject:@"menu"]) return CaraSkinRect(item[@"frame"]);
    return CGRectMake(0,0,1,1);
}
- (void)layoutSubviews {
    [super layoutSubviews];
    NSDictionary* mapping=self.representation[@"mappingSize"]; if (!mapping) return;
    const double sx=self.bounds.size.width/[mapping[@"width"] doubleValue],sy=self.bounds.size.height/[mapping[@"height"] doubleValue];
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    NSUInteger index=0;
    for (NSDictionary* item in self.representation[@"items"]) {
        CGRect r=CaraSkinRect(item[@"frame"]); NSDictionary* thumb=item[@"thumbstick"];
        CGPoint offset=CGPointZero; BOOL active=NO;
        for (NSDictionary* contact in contacts.allValues) if (contact[@"item"]==item) {
            CGPoint p=[contact[@"point"] CGPointValue];
            offset=[self stickPoint:p frame:r]; active=[self hitItem:p]==item || CGRectContainsPoint(r,p);
        }
        if (thumb) {
            const double w=MIN(r.size.width,MAX(12,[thumb[@"width"] doubleValue]));
            const double h=MIN(r.size.height,MAX(12,[thumb[@"height"] doubleValue]));
            CGRect knob=CGRectMake(CGRectGetMidX(r)-w/2+offset.x*(r.size.width-w)/2,CGRectGetMidY(r)-h/2-offset.y*(r.size.height-h)/2,w,h);
            UIImageView* sprite=knobSprites[index]; sprite.frame=CGRectMake(knob.origin.x*sx,knob.origin.y*sy,knob.size.width*sx,knob.size.height*sy);
        } else if ([highlights[index] isKindOfClass:CAShapeLayer.class]) {
            CAShapeLayer* layer=highlights[index]; layer.frame=CGRectMake(r.origin.x*sx,r.origin.y*sy,r.size.width*sx,r.size.height*sy);
            CGRect rect=CGRectMake(0,0,layer.bounds.size.width,layer.bounds.size.height);
            UIBezierPath* path=[item[@"inputs"] isKindOfClass:NSArray.class] && r.size.width==r.size.height ? [UIBezierPath bezierPathWithOvalInRect:rect] : [UIBezierPath bezierPathWithRoundedRect:rect cornerRadius:12];
            layer.path=path.CGPath; layer.hidden=!active;
        }
        ++index;
    }
    [CATransaction commit];
}
@end
