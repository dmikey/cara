// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/configuration_store.h"
#import "ios/save_sync.h"
#include <cstdio>
#include <cstdlib>
@interface ConfigTestDefaults : NSUserDefaults
@property(strong,nonatomic) NSMutableDictionary* values;
@end
@implementation ConfigTestDefaults
- (instancetype)init { if ((self=[super init])) _values=[NSMutableDictionary new]; return self; }
- (id)objectForKey:(NSString*)key { return self.values[key]; }
- (NSString*)stringForKey:(NSString*)key { return self.values[key]; }
- (void)setObject:(id)value forKey:(NSString*)key { self.values[key]=value; }
- (void)setBool:(BOOL)value forKey:(NSString*)key { self.values[key]=@(value); }
- (void)removeObjectForKey:(NSString*)key { [self.values removeObjectForKey:key]; }
@end
static void Check(BOOL ok,NSString* text) { if (!ok) { NSLog(@"FAIL: %@",text); std::exit(1); } }
int main() { @autoreleasepool {
    NSURL* base=[NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString] isDirectory:YES];
    NSURL* a=[base URLByAppendingPathComponent:@"A"],*b=[base URLByAppendingPathComponent:@"B"],*cloud=[base URLByAppendingPathComponent:@"Cloud"];
    ConfigTestDefaults* A=[ConfigTestDefaults new],*B=[ConfigTestDefaults new];
    CaraConfigurationStore* first=[[CaraConfigurationStore alloc] initWithDocuments:a defaults:A],*second=[[CaraConfigurationStore alloc] initWithDocuments:b defaults:B];
    NSError* error=nil; BOOL changed=NO;
    Check([second exportPreferencesChanged:&changed error:&error] && !changed,@"fresh defaults do not create false sync conflicts");
    [A setObject:@{@"13":@"a"} forKey:@"keyboardBindings"]; [A setObject:@.55 forKey:@"portraitSkinOpacity"]; [A setObject:@NO forKey:@"portraitStatsVisible"];
    [A setObject:@"private-rom-location" forKey:@"romsFolderBookmark"]; [A setObject:@"private-token" forKey:@"accountToken"];
    Check([first exportPreferencesChanged:&changed error:&error] && changed,@"portable settings exported");
    Check([first exportPreferencesChanged:&changed error:&error] && !changed,@"unchanged settings avoid sync work");
    NSString* json=[NSString stringWithContentsOfURL:[a URLByAppendingPathComponent:@"cara/settings/controller-preferences.json"] encoding:NSUTF8StringEncoding error:nil];
    Check(![json containsString:@"private-rom-location"] && ![json containsString:@"private-token"],@"locations and credentials excluded");
    NSURL* cheat=[a URLByAppendingPathComponent:@"cara/cheats/0004000000123400.txt"];
    [NSFileManager.defaultManager createDirectoryAtURL:cheat.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    [@"[Fixture]\n*enabled\n00000000 00000000\n" writeToURL:cheat atomically:YES encoding:NSUTF8StringEncoding error:nil];
    CaraSaveSync* SA=[[CaraSaveSync alloc] initWithRoot:[a URLByAppendingPathComponent:@"cara"] folder:cloud device:@"A" content:CaraSyncConfiguration];
    CaraSaveSync* SB=[[CaraSaveSync alloc] initWithRoot:[b URLByAppendingPathComponent:@"cara"] folder:cloud device:@"B" content:CaraSyncConfiguration];
    Check([SA syncStoppedWithResolution:nil error:&error]!=nil,error.description);
    NSDictionary* restored=[SB syncStoppedWithResolution:nil error:&error]; Check(restored && ![restored[@"conflict"] boolValue],@"new device restores without spurious default conflict");
    Check([second applyPreferences:&error],error.description);
    Check([[B objectForKey:@"portraitSkinOpacity"] isEqual:@.55] && [[[B objectForKey:@"keyboardBindings"] objectForKey:@"13"] isEqual:@"a"],@"controller preferences transferred");
    Check([NSFileManager.defaultManager fileExistsAtPath:[b URLByAppendingPathComponent:@"cara/cheats/0004000000123400.txt"].path],@"cheats transferred in configuration channel");
    Check(!CaraIsConfigurationPath(@"settings/secret.json") && !CaraIsConfigurationPath(@"settings/game.3ds") && !CaraIsConfigurationPath(@"settings/../../token.txt"),@"configuration path allowlist");
    NSString* malicious=@"{\"version\":1,\"preferences\":{\"romsFolderBookmark\":\"bad\"},\"skins\":{\"Portrait\":\"builtin\",\"Landscape\":\"builtin\"}}";
    [malicious writeToURL:[b URLByAppendingPathComponent:@"cara/settings/controller-preferences.json"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    Check(![second applyPreferences:&error] && [[B objectForKey:@"portraitSkinOpacity"] isEqual:@.55],@"invalid remote preferences never partially apply");
    [NSFileManager.defaultManager removeItemAtURL:base error:nil];
    puts("PASS: portable controller settings/cheats sync, fresh-device restore, change detection, private-key exclusion and all-or-nothing validation (local folder simulation)");
} }
