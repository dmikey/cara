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
    [NSFileManager.defaultManager removeItemAtURL:temp error:nil];
    puts("PASS: save transfer, ancestry, conflicts, backups, deletion, corruption and ROM/path exclusion (local folder simulation)");
} }
