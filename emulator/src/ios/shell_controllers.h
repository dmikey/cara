// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <UIKit/UIKit.h>
#import "ios/library_store.h"

@interface CaraLibraryController : UIViewController
@property(strong,nonatomic) CaraLibraryStore* store;
@property(copy,nonatomic) void(^openGame)(NSURL*);
@property(copy,nonatomic) void(^addGame)(void);
@property(copy,nonatomic) void(^chooseFolder)(void);
@property(copy,nonatomic) void(^showSettings)(void);
@property(copy,nonatomic) void(^resumeGame)(void);
@property(nonatomic) BOOL canResume;
@property(copy,nonatomic) NSString* syncTitle;
@property(copy,nonatomic) NSString* syncDetail;
@property(nonatomic) BOOL syncing;
- (void)reloadLibrary;
- (void)suspendArtwork;
@end

@interface CaraSettingsController : UITableViewController <UIAdaptivePresentationControllerDelegate>
// Each section has title, footer and rows. Rows use title, subtitle, symbol,
// enabled, optional value/change (switch), and action (dismiss then perform).
@property(copy,nonatomic) NSArray<NSDictionary*>* sections;
@property(copy,nonatomic) void(^finished)(void);
- (void)refresh;
@end

@interface CaraStatsView : UIView
- (void)updateGameFPS:(double)fps speed:(double)speed guestClock:(unsigned)clock adaptive:(BOOL)adaptive;
- (void)showInactiveGuestClock:(unsigned)clock adaptive:(BOOL)adaptive;
@end

@interface CaraQuickSettingsController : UIViewController <UIAdaptivePresentationControllerDelegate>
@property(copy,nonatomic) NSString* playMode;
@property(nonatomic) BOOL modeSelectionEnabled;
@property(nonatomic) BOOL showFPS;
@property(copy,nonatomic) void(^toggleFPS)(BOOL);
@property(copy,nonatomic) void(^chooseMode)(NSString*);
@property(copy,nonatomic) NSArray<NSDictionary*>* actions;
@property(copy,nonatomic) void(^finished)(void);
@end
