// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/save_sync.h"
#include <cstdio>
#include <cstdlib>
static void Check(BOOL ok,NSString* message) { if (!ok) { NSLog(@"FAIL: %@",message); std::exit(1); } }
static NSString* path=@"sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/00040000/00123400/data/00000001/save.dat";
static void Put(NSURL* root,NSString* value) { NSURL* file=[root URLByAppendingPathComponent:path]; [NSFileManager.defaultManager createDirectoryAtURL:file.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil]; Check([value writeToURL:file atomically:YES encoding:NSUTF8StringEncoding error:nil],@"write fixture"); }
static NSString* Get(NSURL* root) { return [NSString stringWithContentsOfURL:[root URLByAppendingPathComponent:path] encoding:NSUTF8StringEncoding error:nil]; }
static NSDictionary* Sync(CaraSaveSync* sync,NSString* resolution) { NSError* error=nil; NSDictionary* result=[sync syncStoppedWithResolution:resolution error:&error]; Check(result!=nil,error.description?:@"sync"); return result; }
int main() { @autoreleasepool {
    NSURL* temp=[NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString] isDirectory:YES];
    NSURL* a=[temp URLByAppendingPathComponent:@"A"],*b=[temp URLByAppendingPathComponent:@"B"],*cloud=[temp URLByAppendingPathComponent:@"Cloud"];
    CaraSaveSync* A=[[CaraSaveSync alloc] initWithRoot:a folder:cloud device:@"A"];
    CaraSaveSync* B=[[CaraSaveSync alloc] initWithRoot:b folder:cloud device:@"B"];
    Check(CaraIsSavePath(path),@"save recognized");
    Check(!CaraIsSavePath([@"../" stringByAppendingString:path]),@"traversal excluded");
    Check(!CaraIsSavePath([path stringByAppendingString:@".3ds"]),@"ROM excluded");
    Put(a,@"first"); Sync(A,nil); Sync(B,nil); Check([Get(b) isEqual:@"first"],@"A to B");
    Put(b,@"second"); Sync(B,nil); Sync(A,nil); Check([Get(a) isEqual:@"second"],@"B to A");
    Put(a,@"local A"); Put(b,@"local B"); Sync(A,nil);
    NSDictionary* conflict=Sync(B,nil); Check([conflict[@"conflict"] boolValue],@"concurrent conflict"); Check([Get(b) isEqual:@"local B"],@"conflict keeps local");
    NSString* choice=conflict[@"versions"][0][@"id"]; Sync(B,choice); Check([Get(b) isEqual:@"local A"],@"explicit remote");
    NSArray* backups=[NSFileManager.defaultManager contentsOfDirectoryAtPath:[b.path stringByAppendingPathComponent:@"SaveBackups"] error:nil]; Check(backups.count>0,@"backup retained");
    Sync(A,nil); [NSFileManager.defaultManager removeItemAtURL:[a URLByAppendingPathComponent:path] error:nil]; Sync(A,nil); Sync(B,nil); Check(Get(b)==nil,@"deletion propagates");
    Put(a,@"valid"); NSDictionary* published=Sync(A,nil);
    NSURL* corrupt=[[[[cloud URLByAppendingPathComponent:@"Cara Saves/Snapshots"] URLByAppendingPathComponent:published[@"snapshot"]] URLByAppendingPathComponent:@"Files"] URLByAppendingPathComponent:path];
    [@"corrupt" writeToURL:corrupt atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSError* error=nil; Check([B syncStoppedWithResolution:nil error:&error]==nil && error!=nil,@"corrupt rejected"); Check(Get(b)==nil,@"corrupt not applied");
    NSString* statePath=@"states/0004000000123400.01.carastate";
    Check(CaraIsStatePath(statePath) && !CaraIsSavePath(statePath),@"separate state namespace");
    Check(CaraIsStatePath(@"states/homebrew-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA.10.carastate"),@"homebrew state recognized");
    for (NSString* invalid in @[@"states/0004000000123400.00.carastate",@"states/0004000000123400.11.carastate",@"states/../0004000000123400.01.carastate",@"states/0004000000123400.01.carastate\n",@"profile-comparison.carastate",@"states/game.3ds"]) Check(!CaraIsStatePath(invalid),@"non-slot or traversal state excluded");
    CaraSaveSync* SA=[[CaraSaveSync alloc] initWithRoot:a folder:cloud device:@"A" statesOnly:YES];
    CaraSaveSync* SB=[[CaraSaveSync alloc] initWithRoot:b folder:cloud device:@"B" statesOnly:YES];
    void(^putState)(NSURL*,NSString*)=^(NSURL* root,NSString* value) {
        NSURL* file=[root URLByAppendingPathComponent:statePath];
        [NSFileManager.defaultManager createDirectoryAtURL:file.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
        Check([value writeToURL:file atomically:YES encoding:NSUTF8StringEncoding error:nil],@"write state fixture");
    };
    NSString*(^getState)(NSURL*)=^NSString*(NSURL* root) { return [NSString stringWithContentsOfURL:[root URLByAppendingPathComponent:statePath] encoding:NSUTF8StringEncoding error:nil]; };
    putState(a,@"state one"); Sync(SA,nil); Sync(SB,nil); Check([getState(b) isEqual:@"state one"],@"state A to B");
    putState(b,@"state two"); Sync(SB,nil); Sync(SA,nil); Check([getState(a) isEqual:@"state two"],@"state B to A");
    putState(a,@"A branch"); putState(b,@"B branch"); Sync(SA,nil);
    NSDictionary* stateConflict=Sync(SB,nil); Check([stateConflict[@"conflict"] boolValue] && [getState(b) isEqual:@"B branch"],@"state conflict keeps local");
    Check([SB.phase isEqual:@"conflict"] && !SB.busy,@"conflict feedback and completed busy state");
    Sync(SB,stateConflict[@"versions"][0][@"id"]); Check([getState(b) isEqual:@"A branch"],@"state conflict resolution");
    Check([[NSFileManager.defaultManager contentsOfDirectoryAtPath:[b.path stringByAppendingPathComponent:@"StateBackups"] error:nil] count]>0,@"state backup retained");
    Sync(SA,nil); [NSFileManager.defaultManager removeItemAtURL:[a URLByAppendingPathComponent:statePath] error:nil]; Sync(SA,nil); Sync(SB,nil); Check(getState(b)==nil,@"state deletion propagates");
    putState(a,@"good state"); NSDictionary* statePublished=Sync(SA,nil);
    NSURL* badState=[[[cloud URLByAppendingPathComponent:@"Cara States/Snapshots"] URLByAppendingPathComponent:statePublished[@"snapshot"]] URLByAppendingPathComponent:[@"Files" stringByAppendingPathComponent:statePath]];
    [@"damaged state" writeToURL:badState atomically:YES encoding:NSUTF8StringEncoding error:nil]; error=nil;
    Check([SB syncStoppedWithResolution:nil error:&error]==nil && error!=nil && getState(b)==nil,@"corrupt state rejected before local replacement");
    Check([SB.phase isEqual:@"error"] && !SB.busy,@"state error feedback");
    NSURL* largeRoot=[temp URLByAppendingPathComponent:@"LargeLocal"];
    NSURL* largeFile=[largeRoot URLByAppendingPathComponent:statePath];
    [NSFileManager.defaultManager createDirectoryAtURL:largeFile.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    [NSFileManager.defaultManager createFileAtPath:largeFile.path contents:nil attributes:nil];
    NSFileHandle* sparse=[NSFileHandle fileHandleForWritingToURL:largeFile error:nil]; [sparse truncateFileAtOffset:129ull*1024*1024]; [sparse closeFile];
    CaraSaveSync* large=[[CaraSaveSync alloc] initWithRoot:largeRoot folder:[temp URLByAppendingPathComponent:@"LargeCloud"] device:@"large" statesOnly:YES];
    Check([Sync(large,nil)[@"files"] unsignedIntValue]==1,@"state larger than old 128 MiB limit transported");
    Check([large.phase isEqual:@"folderReady"] && !large.cloudUploadVerified && large.lastFolderWrite && !large.busy,@"local folder feedback never claims iCloud upload");
    sparse=[NSFileHandle fileHandleForWritingToURL:largeFile error:nil]; [sparse truncateFileAtOffset:768ull*1024*1024+257]; [sparse closeFile]; error=nil;
    Check([large syncStoppedWithResolution:nil error:&error]==nil && error!=nil,@"oversized state rejected before reading payload");
    [NSFileManager.defaultManager removeItemAtURL:temp error:nil];
    puts("PASS: saves and states transfer, separate histories, conflicts, backups, deletion, corruption, large-state limits, feedback and ROM/path exclusion (local folder simulation)");
} }
