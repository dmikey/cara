// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>

@interface CaraLibraryGame : NSObject
@property(copy,nonatomic) NSURL* url;
@property(copy,nonatomic) NSString* title;
@property(copy,nonatomic) NSString* source;
@property(strong,nonatomic) NSDate* modified;
@property(nonatomic) unsigned long long fileSize;
@end

@interface CaraLibraryStore : NSObject
@property(readonly,nonatomic) NSURL* defaultFolder;
@property(copy,readonly,nonatomic) NSString* folderName;
@property(readonly,nonatomic) BOOL hasCustomFolder;
@property(copy,readonly,nonatomic) NSString* locationError;
- (instancetype)initWithDocuments:(NSURL*)documents defaults:(NSUserDefaults*)defaults;
- (BOOL)chooseFolder:(NSURL*)url error:(NSError**)error;
- (void)useDefaultFolder;
- (BOOL)rememberGame:(NSURL*)url error:(NSError**)error;
- (NSDictionary*)metadataForGame:(NSURL*)url;
- (void)setMetadata:(NSDictionary*)metadata forGame:(NSURL*)url;
- (void)setFavorite:(BOOL)value game:(NSURL*)url;
- (void)recordPlayed:(NSURL*)url;
- (void)removeGame:(NSURL*)url;
- (BOOL)deleteGameFile:(NSURL*)url error:(NSError**)error;
- (void)scan:(void(^)(NSArray<CaraLibraryGame*>*,NSString*))completion;
+ (BOOL)isGameURL:(NSURL*)url;
@end
