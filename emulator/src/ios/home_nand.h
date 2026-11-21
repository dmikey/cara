// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>

@interface CaraHomeNAND : NSObject
@property(readonly,nonatomic) NSURL* menuURL;
@property(readonly,nonatomic) unsigned region;
@property(readonly,nonatomic) uint64_t titleID;
+ (instancetype)installedMenu;
+ (instancetype)importFolder:(NSURL*)folder error:(NSError**)error;
@end
