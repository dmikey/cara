// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>
#import "ios/skin_layout.h"

@interface CaraSkinControls : UIView
@property(nonatomic,strong) CaraSkin* skin;
@property(nonatomic,strong) NSDictionary* representation;
@property(nonatomic,copy) void (^inputChanged)(NSSet<NSString*>*,CGPoint,CGPoint,CGPoint);
@property(nonatomic,copy) void (^menuPressed)(void);
@property(nonatomic,readonly) CGRect upperScreen;
@property(nonatomic,readonly) CGRect lowerScreen;
@property(nonatomic,readonly) UIImage* artwork;
- (void)reset;
- (CGRect)menuFrame;
- (void)updateContact:(NSValue*)identifier atPoint:(CGPoint)point;
- (void)endContact:(NSValue*)identifier;
@end
