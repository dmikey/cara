// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>

@interface CaraSaveSync : NSObject
@property(atomic,readonly) BOOL enabled;
@property(atomic,copy,readonly) NSString* status;
- (instancetype)initWithRoot:(NSURL*)root;
// Direct folder injection is also used by the ROM-free two-device regression test.
- (instancetype)initWithRoot:(NSURL*)root folder:(NSURL*)folder device:(NSString*)device;
- (BOOL)connectFolder:(NSURL*)url error:(NSError**)error;
- (void)disconnect;
- (void)requestSync:(BOOL)allowRestore resolution:(NSString*)resolution completion:(void(^)(NSDictionary*,NSError*))completion;
// Called only by the paused emulation worker after a committed save, or after shutdown.
- (void)captureCommittedSave;
- (NSDictionary*)syncStoppedWithResolution:(NSString*)resolution error:(NSError**)error;
@end

BOOL CaraIsSavePath(NSString* relative);
