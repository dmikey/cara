// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>

@class CaraLibraryGame;

@interface CaraHomeScreen : NSObject
@property(readonly,nonatomic) UIView* upperScreen;
@property(readonly,nonatomic) UIView* lowerScreen;
@property(copy,nonatomic) void (^play)(NSURL*);
@property(copy,nonatomic) void (^addGame)(void);
@property(copy,nonatomic) void (^settings)(void);
@property(copy,nonatomic) void (^resumeGame)(void);
- (void)updateGames:(NSArray<CaraLibraryGame*>*)games detail:(NSString*)detail canResume:(BOOL)canResume;
- (void)handleButtons:(NSSet<NSString*>*)buttons circle:(CGPoint)circle;
- (void)resetInput;
@end
