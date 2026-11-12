// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>

@interface CaraConfigurationStore : NSObject
- (instancetype)initWithDocuments:(NSURL*)documents defaults:(NSUserDefaults*)defaults;
// Exports only portable controls/skin settings; never folder bookmarks, ROM
// locations, account credentials or device-local emulation/audio preferences.
- (BOOL)exportPreferencesChanged:(BOOL*)changed error:(NSError**)error;
- (BOOL)applyPreferences:(NSError**)error;
@end
