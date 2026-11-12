// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

@interface CaraSkin : NSObject
@property(nonatomic,readonly) NSDictionary* info;
@property(nonatomic,readonly) NSDictionary<NSString*,NSData*>* assets;
+ (instancetype)loadURL:(NSURL*)url error:(NSError**)error;
- (NSDictionary*)strictRepresentationForPad:(BOOL)pad landscape:(BOOL)landscape;
- (NSDictionary*)representationForPad:(BOOL)pad landscape:(BOOL)landscape;
@end

CGRect CaraSkinRect(NSDictionary* value);
CGRect CaraAspectFit(CGSize source, CGRect destination);
BOOL CaraValidateRepresentation(NSDictionary* rep);
CGPoint CaraStickPoint(CGPoint point, CGRect frame);
CGPoint CaraTouchPoint(CGPoint point, CGRect screen);
unsigned CaraDPadDirections(CGPoint point, CGRect frame);
