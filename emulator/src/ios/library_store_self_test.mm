// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/library_store.h"
#include <cstdio>
#include <cstdlib>

@interface TestLibraryDefaults : NSUserDefaults
@property(strong,nonatomic) NSMutableDictionary* values;
@end
@implementation TestLibraryDefaults
- (instancetype)init { if ((self=[super init])) _values=[NSMutableDictionary new]; return self; }
- (id)objectForKey:(NSString*)key { return self.values[key]; }
- (NSData*)dataForKey:(NSString*)key { return self.values[key]; }
- (NSDictionary*)dictionaryForKey:(NSString*)key { return self.values[key]; }
- (NSArray*)arrayForKey:(NSString*)key { return self.values[key]; }
- (void)setObject:(id)value forKey:(NSString*)key { self.values[key]=value; }
- (void)removeObjectForKey:(NSString*)key { [self.values removeObjectForKey:key]; }
@end
static void Check(BOOL ok,NSString* message) { if (!ok) { NSLog(@"FAIL: %@",message); std::exit(1); } }
static void Put(NSURL* file) { [NSFileManager.defaultManager createDirectoryAtURL:file.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil]; Check([[@"fixture" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:file atomically:YES],@"write fixture"); }
static NSArray* Scan(CaraLibraryStore* store) {
    __block NSArray* result=nil;
    [store scan:^(NSArray* games,NSString* error) { result=games; }];
    const auto deadline=[NSDate dateWithTimeIntervalSinceNow:10];
    while (!result && [deadline timeIntervalSinceNow]>0) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.02]];
    Check(result!=nil,@"bounded scan completion"); return result;
}
int main() { @autoreleasepool {
    NSURL* root=[NSURL fileURLWithPath:[@"/private/tmp" stringByAppendingPathComponent:NSUUID.UUID.UUIDString] isDirectory:YES];
    NSURL* docs=[root URLByAppendingPathComponent:@"Documents"], *external=[root URLByAppendingPathComponent:@"External"];
    TestLibraryDefaults* defaults=[TestLibraryDefaults new];
    CaraLibraryStore* store=[[CaraLibraryStore alloc] initWithDocuments:docs defaults:defaults];
    Check([NSFileManager.defaultManager fileExistsAtPath:store.defaultFolder.path],@"default Roms created");
    Put([docs URLByAppendingPathComponent:@"legacy.3ds"]);
    Put([store.defaultFolder URLByAppendingPathComponent:@"Nested/Second.CCI"]);
    Put([store.defaultFolder URLByAppendingPathComponent:@"Third.3dsx"]);
    Put([store.defaultFolder URLByAppendingPathComponent:@"notes.txt"]);
    Put([docs URLByAppendingPathComponent:@"cara/cache/not-a-game.elf"]);
    Put([store.defaultFolder URLByAppendingPathComponent:@".hidden.3ds"]);
    Check(Scan(store).count==3,@"recursive Roms and legacy root; hidden and unrelated paths excluded");
    Put([external URLByAppendingPathComponent:@"Fourth.cxi"]);
    NSError* error=nil; Check([store chooseFolder:external error:&error],error.description?:@"choose folder");
    Check(store.hasCustomFolder && Scan(store).count==4,@"external folder augments local collection");
    store=[[CaraLibraryStore alloc] initWithDocuments:docs defaults:defaults];
    Check(store.hasCustomFolder && Scan(store).count==4,@"folder bookmark survives store relaunch");
    NSURL* opened=[root URLByAppendingPathComponent:@"Elsewhere/Opened.elf"]; Put(opened);
    Check([store rememberGame:opened error:&error],@"remember file opened outside folder");
    Check([store rememberGame:opened error:&error],@"remember duplicate");
    Check(Scan(store).count==5 && [[defaults arrayForKey:@"libraryGameBookmarks"] count]==1,@"deduplicated collection and records");
    Check(![store rememberGame:[NSURL fileURLWithPath:@"/private/tmp/not-a-rom.zip"] error:&error],@"ZIP remains unsupported");
    [store useDefaultFolder]; Check(!store.hasCustomFolder && Scan(store).count==4,@"return to default keeps local and explicitly opened games");
    Check([NSFileManager.defaultManager fileExistsAtPath:[external URLByAppendingPathComponent:@"Fourth.cxi"].path],@"changing folder never deletes/moves game");
    NSURL* local=[store.defaultFolder URLByAppendingPathComponent:@"Third.3dsx"];
    [store setFavorite:YES game:local]; [store recordPlayed:local];
    Check([[store metadataForGame:local][@"favorite"] boolValue] && [store metadataForGame:local][@"lastPlayed"],@"real per-game favorite and recent metadata");
    NSURL* save=[docs URLByAppendingPathComponent:@"cara/sdmc/save.dat"]; Put(save);
    NSURL* state=[docs URLByAppendingPathComponent:@"cara/states/example.carastate"]; Put(state);
    [store removeGame:local]; Check(Scan(store).count==3 && [NSFileManager.defaultManager fileExistsAtPath:local.path],@"removal hides scanned games without deleting ROM");
    store=[[CaraLibraryStore alloc] initWithDocuments:docs defaults:defaults]; Check(Scan(store).count==3,@"hidden games survive relaunch");
    Check([store rememberGame:local error:&error] && Scan(store).count==4,@"explicit re-add restores hidden file");
    Check([store deleteGameFile:local error:&error] && ![NSFileManager.defaultManager fileExistsAtPath:local.path] && Scan(store).count==3,@"explicit delete removes only requested fixture ROM");
    Check([NSFileManager.defaultManager fileExistsAtPath:save.path] && [NSFileManager.defaultManager fileExistsAtPath:state.path],@"game removal and deletion preserve saves and states");
    [NSFileManager.defaultManager removeItemAtURL:root error:nil];
    puts("PASS: Roms creation, nested/legacy discovery, bookmarks, external file registration, deduplication and non-destructive folder selection (local simulation)");
} }
