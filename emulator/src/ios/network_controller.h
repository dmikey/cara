// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once
#import <UIKit/UIKit.h>

@interface CaraNetworkController : UITableViewController
@property(copy, nonatomic) NSDictionary* (^connectionStatus)(void);
@property(copy, nonatomic) void (^joinRoom)(NSString* endpoint, NSString* room, NSString* password);
@property(copy, nonatomic) void (^leaveRoom)(void);
@end
