// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>

FOUNDATION_EXPORT NSNotificationName const CaraSaveSyncDidChangeNotification;

typedef NS_ENUM(NSUInteger,CaraSyncContent) { CaraSyncGameSaves, CaraSyncSaveStates, CaraSyncConfiguration };

@interface CaraSaveSync : NSObject
@property(atomic,readonly) BOOL enabled;
@property(atomic,copy,readonly) NSString* status;
@property(atomic,copy,readonly) NSString* detail;
@property(atomic,copy,readonly) NSString* phase;
@property(atomic,readonly) BOOL busy;
@property(atomic,readonly) BOOL cloudUploadVerified;
@property(atomic,strong,readonly) NSDate* lastFolderWrite;
@property(nonatomic,readonly) BOOL statesOnly;
@property(nonatomic,readonly) BOOL configurationOnly;
@property(nonatomic,readonly) CaraSyncContent content;
@property(copy,nonatomic,readonly) NSString* contentName;
- (instancetype)initWithRoot:(NSURL*)root;
- (instancetype)initWithRoot:(NSURL*)root content:(CaraSyncContent)content;
- (instancetype)initWithRoot:(NSURL*)root folder:(NSURL*)folder device:(NSString*)device content:(CaraSyncContent)content;
- (instancetype)initWithRoot:(NSURL*)root statesOnly:(BOOL)statesOnly;
// Direct folder injection is also used by the ROM-free two-device regression test.
- (instancetype)initWithRoot:(NSURL*)root folder:(NSURL*)folder device:(NSString*)device;
- (instancetype)initWithRoot:(NSURL*)root folder:(NSURL*)folder device:(NSString*)device statesOnly:(BOOL)statesOnly;
- (void)refreshCloudStatus;
- (BOOL)connectFolder:(NSURL*)url error:(NSError**)error;
- (void)disconnect;
- (void)requestSync:(BOOL)allowRestore resolution:(NSString*)resolution completion:(void(^)(NSDictionary*,NSError*))completion;
// Called only by the paused emulation worker after a committed save, or after shutdown.
- (void)captureCommittedSave;
- (NSDictionary*)syncStoppedWithResolution:(NSString*)resolution error:(NSError**)error;
@end

BOOL CaraIsSavePath(NSString* relative);

BOOL CaraIsStatePath(NSString* relative);

BOOL CaraIsConfigurationPath(NSString* relative);
