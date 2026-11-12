// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>

@interface CaraStateRecord : NSObject
@property(copy,nonatomic) NSString* filename;
@property(copy,nonatomic) NSString* name;
@property(copy,nonatomic) NSString* kind;
@property(strong,nonatomic) NSDate* date;
@property(nonatomic) BOOL locked;
@property(nonatomic) BOOL compatible;
@end

@interface CaraStateCatalog : NSObject
@property(copy,readonly,nonatomic) NSString* gameID;
@property(copy,readonly,nonatomic) NSURL* directory;
- (instancetype)initWithDirectory:(NSURL*)directory gameID:(NSString*)gameID fingerprint:(NSString*)fingerprint gameHash:(NSData*)gameHash new3DS:(BOOL)new3DS backend:(unsigned)backend;
- (NSArray<CaraStateRecord*>*)records;
- (NSString*)newFilename:(NSString*)kind;
- (BOOL)prepareToSave:(NSString*)filename error:(NSError**)error;
- (void)didSave:(NSString*)filename name:(NSString*)name kind:(NSString*)kind;
- (BOOL)setLocked:(BOOL)locked filename:(NSString*)filename error:(NSError**)error;
- (BOOL)rename:(NSString*)filename name:(NSString*)name error:(NSError**)error;
- (BOOL)remove:(NSString*)filename error:(NSError**)error;
- (NSURL*)urlForFilename:(NSString*)filename;
@end
