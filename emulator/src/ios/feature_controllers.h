// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>
#import "ios/input_hub.h"
#import "ios/state_catalog.h"

@interface CaraMappingController : UITableViewController
@property(strong,nonatomic) CaraInputHub* hub;
@property(nonatomic) BOOL keyboard;
@property(copy,nonatomic) void(^finished)(void);
@end

@interface CaraStateManagerController : UITableViewController
@property(strong,nonatomic) CaraStateCatalog* catalog;
@property(copy,nonatomic) void(^save)(NSString*,NSString*);
@property(copy,nonatomic) void(^load)(NSString*);
@property(copy,nonatomic) void(^changed)(void);
@property(copy,nonatomic) void(^finished)(void);
- (void)reloadStates;
@end

FOUNDATION_EXPORT NSString* CaraInputActionTitle(NSString* action);
