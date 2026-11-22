// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>

@interface CaraNearbyStreetPass : NSObject
// Owner calls start/stop on the main thread at library/game and scene transitions.
- (void)startWithNand:(NSURL*)nand;
- (void)stop;
@property(nonatomic, readonly) NSString* statusText;
@end
