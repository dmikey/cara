// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/state_catalog.h"
#import "ios/save_sync.h"
#include <cstring>

@implementation CaraStateRecord
@end
static NSError* StateError(NSString* text) { return [NSError errorWithDomain:@"CaraStateLibrary" code:1 userInfo:@{NSLocalizedDescriptionKey:text}]; }

@implementation CaraStateCatalog {
    NSString* engine;
    NSData* expectedGameHash;
    BOOL expectedNew3DS;
    unsigned expectedBackend;
    NSMutableDictionary* metadata;
    BOOL metadataValid;
}
@synthesize directory=_directory,gameID=_gameID;
- (instancetype)initWithDirectory:(NSURL*)directory gameID:(NSString*)identifier fingerprint:(NSString*)fingerprint gameHash:(NSData*)gameHash new3DS:(BOOL)new3DS backend:(unsigned)backend {
    if ((self=[super init])) {
        _directory=directory; _gameID=[identifier copy]; engine=fingerprint; expectedGameHash=gameHash; expectedNew3DS=new3DS; expectedBackend=backend;
        [NSFileManager.defaultManager createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:nil];
        [self readMetadata];
    } return self;
}
- (void)readMetadata {
    NSURL* url=[_directory URLByAppendingPathComponent:@"metadata.json"];
    metadata=[NSMutableDictionary new]; metadataValid=YES;
    if (![NSFileManager.defaultManager fileExistsAtPath:url.path]) return;
    NSNumber* size=nil; [url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    if (!size || size.unsignedLongLongValue>2*1024*1024) { metadataValid=NO; return; }
    NSData* bytes=[NSData dataWithContentsOfURL:url]; id parsed=bytes?[NSJSONSerialization JSONObjectWithData:bytes options:NSJSONReadingMutableContainers error:nil]:nil;
    if (![parsed isKindOfClass:NSDictionary.class] || [parsed count]>4096) { metadataValid=NO; return; }
    for (NSString* filename in parsed) {
        NSDictionary* row=parsed[filename];
        if (!CaraIsStatePath([@"states" stringByAppendingPathComponent:filename]) || ![row isKindOfClass:NSDictionary.class] || ((row[@"name"] && (![row[@"name"] isKindOfClass:NSString.class] || [row[@"name"] length]>100)) || (row[@"locked"] && ![row[@"locked"] isKindOfClass:NSNumber.class]))) { metadataValid=NO; return; }
        metadata[filename]=[row mutableCopy];
    }
}
- (BOOL)writeMetadata:(NSError**)error {
    if (!metadataValid) { if (error) *error=StateError(@"State labels couldn’t be read. State files were kept; restore the state metadata from a backup before replacing or deleting states."); return NO; }
    NSData* bytes=[NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingSortedKeys error:error];
    return bytes && [bytes writeToURL:[_directory URLByAppendingPathComponent:@"metadata.json"] options:NSDataWritingAtomic error:error];
}
- (NSURL*)urlForFilename:(NSString*)filename {
    if (![filename hasPrefix:[_gameID stringByAppendingString:@"."]] || !CaraIsStatePath([@"states" stringByAppendingPathComponent:filename]) || [filename isEqual:@"metadata.json"]) return nil;
    return [_directory URLByAppendingPathComponent:filename];
}
- (NSArray<CaraStateRecord*>*)records {
    @synchronized(self) {
        [self readMetadata];
        NSMutableArray* records=[NSMutableArray new];
        for (NSURL* url in [NSFileManager.defaultManager contentsOfDirectoryAtURL:_directory includingPropertiesForKeys:@[NSURLIsRegularFileKey,NSURLIsSymbolicLinkKey,NSURLContentModificationDateKey] options:NSDirectoryEnumerationSkipsHiddenFiles error:nil]) {
            NSString* filename=url.lastPathComponent;
            if (![self urlForFilename:filename]) continue;
            NSDictionary* values=[url resourceValuesForKeys:@[NSURLIsRegularFileKey,NSURLIsSymbolicLinkKey,NSURLContentModificationDateKey] error:nil];
            if (![values[NSURLIsRegularFileKey] boolValue] || [values[NSURLIsSymbolicLinkKey] boolValue]) continue;
            NSData* header=nil; NSFileHandle* file=[NSFileHandle fileHandleForReadingFromURL:url error:nil];
            @try { header=[file readDataOfLength:256]; } @catch (NSException* exception) {} @finally { [file closeFile]; }
            CaraStateRecord* record=[CaraStateRecord new]; record.filename=filename; record.date=values[NSURLContentModificationDateKey]?:NSDate.distantPast;
            NSDictionary* row=metadata[filename];
            record.kind=[filename containsString:@".quick."]?@"quick":[filename containsString:@".auto-"]?@"auto":[filename containsString:@".backup-"]?@"backup":@"manual";
            record.name=row[@"name"]?:([record.kind isEqual:@"quick"]?@"Quick save":[record.kind isEqual:@"auto"]?@"Automatic state":[record.kind isEqual:@"backup"]?@"Backup":@"Save state");
            record.locked=!metadataValid || [row[@"locked"] boolValue];
            if (header.length==256) {
                const auto* data=(const uint8_t*)header.bytes;
                NSString* fingerprint=[[NSString alloc] initWithBytes:data+48 length:64 encoding:NSASCIIStringEncoding];
                uint32_t mode=0,backend=0; memcpy(&mode,data+144,4); memcpy(&backend,data+180,4);
                record.compatible=!memcmp(data,"CARAST01",8) && [fingerprint isEqual:engine] && mode==unsigned(expectedNew3DS) && backend==expectedBackend && expectedGameHash.length==32 && !memcmp(data+148,expectedGameHash.bytes,32);
            }
            [records addObject:record];
        }
        return [records sortedArrayUsingComparator:^NSComparisonResult(CaraStateRecord* a,CaraStateRecord* b) { return [b.date compare:a.date]; }];
    }
}
- (NSString*)newFilename:(NSString*)kind {
    if ([kind isEqual:@"quick"]) return [_gameID stringByAppendingString:@".quick.carastate"];
    if (![@[@"auto",@"manual",@"backup"] containsObject:kind]) return nil;
    return [NSString stringWithFormat:@"%@.%@-%@.carastate",_gameID,kind,NSUUID.UUID.UUIDString];
}
- (BOOL)prepareToSave:(NSString*)filename error:(NSError**)error {
    @synchronized(self) {
        [self readMetadata]; NSURL* file=[self urlForFilename:filename];
        if (!file) { if (error) *error=StateError(@"Invalid state location."); return NO; }
        if (!metadataValid || [metadata[filename][@"locked"] boolValue]) { if (error) *error=StateError(@"This state is locked. Unlock it before replacing it."); return NO; }
        if ([NSFileManager.defaultManager fileExistsAtPath:file.path]) {
            NSString* backup=[self newFilename:@"backup"];
            if (![NSFileManager.defaultManager copyItemAtURL:file toURL:[self urlForFilename:backup] error:error]) return NO;
            metadata[backup]=[@{@"name":@"Before quick save",@"locked":@NO} mutableCopy];
            if (![self writeMetadata:error]) return NO;
        }
        return YES;
    }
}
- (void)didSave:(NSString*)filename name:(NSString*)name kind:(NSString*)kind {
    @synchronized(self) {
        [self readMetadata]; if (![self urlForFilename:filename] || !metadataValid) return;
        name=[name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (name.length>100) { NSRange last=[name rangeOfComposedCharacterSequenceAtIndex:99]; name=[name substringToIndex:NSMaxRange(last)<=100?100:last.location]; }
        metadata[filename]=[@{@"name":name.length?name:@"Save state",@"locked":@NO} mutableCopy]; [self writeMetadata:nil];
        // Keep three rolling auto states and three replacement backups. Locks
        // opt a state out of automatic cleanup, including across cloud restores.
        for (NSString* category in @[@"auto",@"backup"]) {
            unsigned kept=0;
            for (CaraStateRecord* record in [self records]) if ([record.kind isEqual:category] && !record.locked) {
                if (++kept>3) { [NSFileManager.defaultManager removeItemAtURL:[self urlForFilename:record.filename] error:nil]; [metadata removeObjectForKey:record.filename]; }
            }
            [self writeMetadata:nil];
        }
        [self writeMetadata:nil];
    }
}
- (BOOL)setLocked:(BOOL)locked filename:(NSString*)filename error:(NSError**)error {
    @synchronized(self) {
        [self readMetadata]; if (![self urlForFilename:filename] || !metadataValid) { if (error) *error=StateError(@"State metadata is unavailable."); return NO; }
        NSMutableDictionary* row=[metadata[filename] mutableCopy]?:[NSMutableDictionary new]; row[@"locked"]=@(locked); metadata[filename]=row;
        return [self writeMetadata:error];
    }
}
- (BOOL)rename:(NSString*)filename name:(NSString*)name error:(NSError**)error {
    @synchronized(self) {
        [self readMetadata]; name=[name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (![self urlForFilename:filename] || !metadataValid || !name.length || name.length>100) { if (error) *error=StateError(@"Choose a state name up to 100 characters."); return NO; }
        NSMutableDictionary* row=[metadata[filename] mutableCopy]?:[NSMutableDictionary new]; row[@"name"]=name; metadata[filename]=row; return [self writeMetadata:error];
    }
}
- (BOOL)remove:(NSString*)filename error:(NSError**)error {
    @synchronized(self) {
        [self readMetadata]; NSURL* file=[self urlForFilename:filename];
        if (!file || !metadataValid || [metadata[filename][@"locked"] boolValue]) { if (error) *error=StateError(@"Unlock this state before deleting it."); return NO; }
        if (![NSFileManager.defaultManager removeItemAtURL:file error:error]) return NO;
        [metadata removeObjectForKey:filename]; return [self writeMetadata:error];
    }
}
@end
