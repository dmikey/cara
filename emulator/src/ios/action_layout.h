// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>
#import "ios/skin_layout.h"

// Versions 1/2: normalized safe-canvas frames, independent portrait/landscape variants.
FOUNDATION_EXPORT NSDictionary* CaraNewActionLayout(void);
FOUNDATION_EXPORT BOOL CaraValidateActionLayout(id value, NSError** error);
FOUNDATION_EXPORT NSArray<NSString*>* CaraActionBindings(NSDictionary* control);
FOUNDATION_EXPORT NSDictionary* CaraActionLayoutFromSkin(CaraSkin* skin, NSError** error);
FOUNDATION_EXPORT NSURL* CaraActionPresetDirectory(void);
// Validate and atomically register an authored layout. Imported/copied layouts
// must remove registrationID first, so another preset cannot be overwritten.
FOUNDATION_EXPORT NSDictionary* CaraRegisterActionLayout(NSDictionary* layout, NSURL* directory, NSError** error);
FOUNDATION_EXPORT NSData* CaraActionSkinData(NSDictionary* layout, NSError** error);
FOUNDATION_EXPORT CGRect CaraActionFrame(NSDictionary* control, CGSize size);
// Navigation rows must retain their presenting sheet while pushing the editor.
FOUNDATION_EXPORT NSDictionary* CaraActionLayoutSettingsRow(void(^openEditor)(void));

@interface CaraActionControls : UIView
@property(nonatomic,copy) NSArray<NSDictionary*>* controls;
// Editor-only boundary rendering. Hidden zones remain fully interactive in play.
@property(nonatomic) BOOL showsHiddenControls;
@property(nonatomic,copy) void(^inputChanged)(NSSet<NSString*>*,CGPoint,CGPoint);
- (void)updateContact:(NSValue*)identifier atPoint:(CGPoint)point;
- (void)endContact:(NSValue*)identifier;
- (void)reset;
@end

@interface CaraActionLayoutEditor : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic,copy) NSDictionary* layout;
@property(nonatomic,copy) NSString* presetName;
@property(nonatomic,strong) CaraSkin* skin;
@property(nonatomic,strong) CaraSkin* landscapeSkin;
@property(nonatomic,copy) void(^saved)(NSDictionary*);
@end
