// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <GameController/GameController.h>

FOUNDATION_EXPORT NSNotificationName const CaraInputHubDidChangeNotification;
@interface CaraInputHub : NSObject
@property(nonatomic) BOOL active;
@property(nonatomic,readonly) CGPoint touchPoint;
// Surface sources merge with hardware; clearing one source cannot release another.
- (void)updateSurface:(NSString*)source buttons:(NSSet<NSString*>*)buttons circle:(CGPoint)circle cstick:(CGPoint)cstick touch:(CGPoint)touch;
- (void)clearSurfaces;
@property(copy,nonatomic) void(^inputChanged)(NSSet<NSString*>*,CGPoint,CGPoint);
@property(copy,nonatomic) void(^commandChanged)(NSString*,BOOL);
@property(copy,readonly,nonatomic) NSString* controllerName;
@property(readonly,nonatomic) BOOL keyboardConnected;
@property(readonly,nonatomic) NSArray<NSString*>* actions;
- (instancetype)initWithDefaults:(NSUserDefaults*)defaults;
- (instancetype)initWithDefaults:(NSUserDefaults*)defaults observesDevices:(BOOL)observesDevices;
- (void)useController:(GCController*)controller;
- (NSString*)bindingForAction:(NSString*)action keyboard:(BOOL)keyboard;
- (void)captureAction:(NSString*)action keyboard:(BOOL)keyboard completion:(void(^)(NSError*))completion;
- (void)cancelCapture;
- (void)resetMappings:(BOOL)keyboard;
- (void)reloadMappings;
// Shared with native synthetic-input regressions; hardware events call this path.
- (void)handleKeyboardCode:(GCKeyCode)code pressed:(BOOL)pressed;
@end
