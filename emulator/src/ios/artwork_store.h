// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>

// All methods run on the library's serial artwork queue, never the emulation thread.
@interface CaraArtworkStore : NSObject
- (instancetype)initWithRoot:(NSURL*)root;
- (NSDictionary*)artworkForURL:(NSURL*)game title:(NSString*)title;
- (BOOL)importImage:(NSURL*)image forGame:(NSURL*)game hero:(BOOL)hero error:(NSError**)error;
- (void)removeOverridesForGame:(NSURL*)game;
+ (NSString*)normalizedTitle:(NSString*)title;
+ (NSString*)matchName:(NSArray<NSString*>*)names game:(NSURL*)game title:(NSString*)title;
@end
