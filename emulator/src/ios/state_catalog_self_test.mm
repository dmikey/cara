// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/state_catalog.h"
#import "ios/save_sync.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static void Check(BOOL ok,NSString* text) { if (!ok) { NSLog(@"FAIL: %@",text); std::exit(1); } }
static void Fixture(NSURL* url,NSString* fingerprint) {
    NSMutableData* data=[NSMutableData dataWithLength:256]; auto* header=(uint8_t*)data.mutableBytes;
    memcpy(header,"CARAST01",8); header[8]=1; header[13]=1; memcpy(header+48,[fingerprint dataUsingEncoding:NSASCIIStringEncoding].bytes,64);
    Check([data writeToURL:url atomically:YES],@"write synthetic state header");
}
int main() { @autoreleasepool {
    NSURL* directory=[NSURL fileURLWithPath:[@"/private/tmp" stringByAppendingPathComponent:NSUUID.UUID.UUIDString] isDirectory:YES];
    NSString* fingerprint=[@"f" stringByPaddingToLength:64 withString:@"f" startingAtIndex:0];
    CaraStateCatalog* catalog=[[CaraStateCatalog alloc] initWithDirectory:directory gameID:@"0004000000123400" fingerprint:fingerprint gameHash:[NSMutableData dataWithLength:32] new3DS:NO backend:0];
    NSString* manual=[catalog newFilename:@"manual"]; NSError* error=nil;
    Check([catalog prepareToSave:manual error:&error],@"new manual state permitted"); Fixture([catalog urlForFilename:manual],fingerprint); [catalog didSave:manual name:@"Before the boss" kind:@"manual"];
    Check(catalog.records.count==1 && catalog.records.firstObject.compatible,@"compatible state found by game identity");
    Check([catalog setLocked:YES filename:manual error:&error],@"lock state");
    Check(![catalog prepareToSave:manual error:&error] && ![catalog remove:manual error:&error],@"locked state cannot be overwritten or deleted");
    Check([catalog rename:manual name:@"Protected checkpoint" error:&error],@"rename retains identity");
    NSString* quick=[catalog newFilename:@"quick"]; Fixture([catalog urlForFilename:quick],fingerprint); [catalog didSave:quick name:@"Quick save" kind:@"quick"];
    Check([catalog prepareToSave:quick error:&error],@"quick overwrite makes backup");
    Check([[catalog.records filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"kind == 'backup'"]] count]==1,@"replacement backup retained");
    NSString* protectedAuto=nil;
    for (unsigned i=0;i<6;++i) {
        NSString* name=[catalog newFilename:@"auto"]; Check(CaraIsStatePath([@"states" stringByAppendingPathComponent:name]),@"auto state sync policy");
        Fixture([catalog urlForFilename:name],fingerprint); [catalog didSave:name name:@"Automatic state" kind:@"auto"];
        if (!i) { protectedAuto=name; [catalog setLocked:YES filename:name error:&error]; }
    }
    Check([NSFileManager.defaultManager fileExistsAtPath:[catalog urlForFilename:protectedAuto].path],@"locked automatic state survives rolling cleanup");
    Check([[catalog.records filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"kind == 'auto' AND locked == NO"]] count]==3,@"only three unlocked automatic states retained");
    Check([catalog urlForFilename:@"../other.carastate"]==nil && [catalog urlForFilename:@"0004000000999999.01.carastate"]==nil,@"foreign and traversal filenames rejected");
    CaraStateCatalog* other=[[CaraStateCatalog alloc] initWithDirectory:directory gameID:@"0004000000123400" fingerprint:[@"a" stringByPaddingToLength:64 withString:@"a" startingAtIndex:0] gameHash:[NSMutableData dataWithLength:32] new3DS:NO backend:0];
    Check(!other.records.firstObject.compatible,@"incompatible build retained and marked");
    [@"broken" writeToURL:[directory URLByAppendingPathComponent:@"metadata.json"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    Check(![catalog prepareToSave:quick error:&error] && ![catalog remove:quick error:&error],@"corrupt labels never permit silent overwrite or deletion");
    [NSFileManager.defaultManager removeItemAtURL:directory error:nil];
    puts("PASS: per-game state identity, compatibility, names, locks, quick-save backup, rolling autosaves and safe metadata failure (synthetic headers)");
} }
