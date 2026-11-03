// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/save_sync.h"
#import <CommonCrypto/CommonDigest.h>
#include <cstdlib>
#include <limits.h>

static NSError* SaveError(NSString* message) { return [NSError errorWithDomain:@"CaraSaveSync" code:1 userInfo:@{NSLocalizedDescriptionKey:message}]; }
static BOOL SafeID(NSString* value) {
    if (![value isKindOfClass:NSString.class] || !value.length || value.length>128) return NO;
    return [value rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"] invertedSet]].location==NSNotFound;
}
BOOL CaraIsSavePath(NSString* relative) {
    if (![relative isKindOfClass:NSString.class] || relative.length>2048 || [relative hasPrefix:@"/"] || [relative containsString:@"\\"]) return NO;
    NSArray* p=relative.pathComponents;
    if ([p containsObject:@".."] || [p containsObject:@"."]) return NO;
    if ([@[@"3ds",@"cci",@"cxi",@"cia",@"3dsx",@"nds",@"elf",@"zip",@"deltaskin"] containsObject:relative.pathExtension.lowercaseString]) return NO;
    if (p.count>=9 && [p[0] isEqual:@"sdmc"] && [p[1] isEqual:@"Nintendo 3DS"] && [p[4] isEqual:@"title"] && [p[7] isEqual:@"data"]) return YES;
    if (p.count>=8 && [p[0] isEqual:@"sdmc"] && [p[1] isEqual:@"Nintendo 3DS"] && [p[4] isEqual:@"extdata"]) return YES;
    return p.count>=7 && [p[0] isEqual:@"nand"] && [p[1] isEqual:@"data"] && [p[3] isEqual:@"extdata"];
}
static NSString* HashFile(NSURL* url,NSError** error) {
    NSDictionary* values=[url resourceValuesForKeys:@[NSURLIsRegularFileKey,NSURLIsSymbolicLinkKey,NSURLFileSizeKey] error:error];
    if (!values || ![values[NSURLIsRegularFileKey] boolValue] || [values[NSURLIsSymbolicLinkKey] boolValue] || [values[NSURLFileSizeKey] unsignedLongLongValue]>128ull*1024*1024) { if (error && !*error) *error=SaveError(@"A save file is invalid or too large."); return nil; }
    NSFileHandle* file=[NSFileHandle fileHandleForReadingFromURL:url error:error]; if (!file) return nil;
    CC_SHA256_CTX context; CC_SHA256_Init(&context);
    @try { while (YES) { @autoreleasepool { NSData* data=[file readDataOfLength:65536]; if (!data.length) break; CC_SHA256_Update(&context,data.bytes,(CC_LONG)data.length); } } }
    @catch (NSException* exception) { [file closeFile]; if (error) *error=SaveError(@"Could not read save data."); return nil; }
    [file closeFile]; unsigned char digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(digest,&context);
    NSMutableString* hash=[NSMutableString new]; for (unsigned char byte:digest) [hash appendFormat:@"%02x",byte]; return hash;
}
static BOOL MakeDirectory(NSURL* url,NSError** error) { return [NSFileManager.defaultManager createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:error]; }
static NSDictionary* ReadJSON(NSURL* url,NSError** error,BOOL coordinated) {
    __block NSData* data=nil; __block NSError* failure=nil;
    void (^read)(NSURL*)=^(NSURL* actual) { data=[NSData dataWithContentsOfURL:actual options:0 error:&failure]; };
    if (coordinated) [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateReadingItemAtURL:url options:0 error:&failure byAccessor:read]; else read(url);
    if (!data || data.length>4*1024*1024) { if (error) *error=failure?:SaveError(@"Cloud metadata is missing or too large."); return nil; }
    id value=[NSJSONSerialization JSONObjectWithData:data options:0 error:&failure];
    if (![value isKindOfClass:NSDictionary.class]) { if (error) *error=failure?:SaveError(@"Cloud metadata is invalid."); return nil; }
    return value;
}
static BOOL WriteJSON(NSDictionary* value,NSURL* url,NSError** error,BOOL coordinated) {
    NSData* data=[NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingPrettyPrinted error:error]; if (!data) return NO;
    __block BOOL ok=NO; __block NSError* failure=nil;
    void (^write)(NSURL*)=^(NSURL* actual) { ok=[data writeToURL:actual options:NSDataWritingAtomic error:&failure]; };
    if (coordinated) [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateWritingItemAtURL:url options:NSFileCoordinatorWritingForReplacing error:&failure byAccessor:write]; else write(url);
    if (!ok && error) *error=failure?:SaveError(@"Could not write save metadata."); return ok;
}
static NSDictionary* Collect(NSURL* root,NSError** error) {
    NSMutableDictionary* files=[NSMutableDictionary new]; unsigned long long total=0;
    for (NSString* sub in @[@"sdmc/Nintendo 3DS",@"nand/data"]) {
        NSURL* base=[root URLByAppendingPathComponent:sub isDirectory:YES];
        if (![NSFileManager.defaultManager fileExistsAtPath:base.path]) continue;
        __block NSError* enumerationError=nil;
        NSDirectoryEnumerator* e=[NSFileManager.defaultManager enumeratorAtURL:base includingPropertiesForKeys:@[NSURLIsRegularFileKey,NSURLIsSymbolicLinkKey,NSURLFileSizeKey] options:0 errorHandler:^BOOL(NSURL* url,NSError* failure) { enumerationError=failure; return NO; }];
        for (NSURL* url in e) {
            if ([url.lastPathComponent isEqual:@"content"]) { [e skipDescendants]; continue; }
            NSString* rootPath=root.path;
            NSString* prefix=[rootPath hasSuffix:@"/"]?rootPath:[rootPath stringByAppendingString:@"/"];
            if (![url.path hasPrefix:prefix]) continue;
            NSString* path=[url.path substringFromIndex:prefix.length]; if (!CaraIsSavePath(path)) continue;
            NSDictionary* info=[url resourceValuesForKeys:@[NSURLIsRegularFileKey,NSURLIsSymbolicLinkKey,NSURLFileSizeKey] error:error];
            if ([info[NSURLIsSymbolicLinkKey] boolValue]) { if (error) *error=SaveError(@"Save links are not supported."); return nil; }
            if (![info[NSURLIsRegularFileKey] boolValue]) continue;
            total+=[info[NSURLFileSizeKey] unsignedLongLongValue];
            if (total>512ull*1024*1024 || files.count>=4096) { if (error) *error=SaveError(@"The save set is too large."); return nil; }
            NSString* hash=HashFile(url,error); if (!hash) return nil;
            files[path]=@{@"hash":hash,@"size":info[NSURLFileSizeKey]};
        }
        if (enumerationError) { if (error) *error=enumerationError; return nil; }
        if (error && *error) return nil;
    }
    return files;
}
static BOOL ValidFiles(NSDictionary* files) {
    if (![files isKindOfClass:NSDictionary.class] || files.count>4096) return NO;
    unsigned long long total=0;
    for (NSString* path in files) {
        NSDictionary* entry=files[path]; if (!CaraIsSavePath(path) || ![entry isKindOfClass:NSDictionary.class]) return NO;
        NSString* hash=entry[@"hash"];
        if (![hash isKindOfClass:NSString.class] || hash.length!=64 || [hash rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location!=NSNotFound || ![entry[@"size"] isKindOfClass:NSNumber.class]) return NO;
        const unsigned long long size=[entry[@"size"] unsignedLongLongValue]; total+=size;
        if (size>128ull*1024*1024 || total>512ull*1024*1024) return NO;
    }
    return YES;
}
static BOOL NoSaveLinks(NSURL* base,NSString* path,NSError** error) {
    NSURL* cursor=base;
    for (NSString* component in path.pathComponents) {
        cursor=[cursor URLByAppendingPathComponent:component];
        NSNumber* linked=nil; [cursor getResourceValue:&linked forKey:NSURLIsSymbolicLinkKey error:nil];
        if (linked.boolValue) { if (error) *error=SaveError(@"A save path contains a link. Local saves were kept."); return NO; }
    }
    return YES;
}
static BOOL CopyFile(NSURL* source,NSURL* destination,NSString* hash,NSError** error,BOOL coordinated) {
    if (!MakeDirectory(destination.URLByDeletingLastPathComponent,error)) return NO;
    __block BOOL ok=NO; __block NSError* failure=nil;
    void (^copy)(NSURL*)=^(NSURL* actual) {
        NSString* actualHash=HashFile(actual,&failure);
        if (![actualHash isEqual:hash]) { if (!failure) failure=SaveError(@"A save changed or its checksum did not match. Local saves were kept."); return; }
        ok=[NSFileManager.defaultManager copyItemAtURL:actual toURL:destination error:&failure];
    };
    if (coordinated) [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateReadingItemAtURL:source options:0 error:&failure byAccessor:copy]; else copy(source);
    if (!ok && error) *error=failure?:SaveError(@"Could not copy save data."); return ok;
}

@interface CaraSaveSync ()
@property(atomic,copy,readwrite) NSString* status;
@end
@implementation CaraSaveSync {
    NSURL* root;
    NSURL* directFolder;
    NSMutableDictionary* state;
    dispatch_queue_t queue;
    BOOL syncing;
}
@synthesize status=_status;
- (instancetype)initWithRoot:(NSURL*)url { return [self initWithRoot:url folder:nil device:nil]; }
- (instancetype)initWithRoot:(NSURL*)url folder:(NSURL*)folder device:(NSString*)device {
    if ((self=[super init])) {
        root=url; directFolder=folder; MakeDirectory(root,nil);
        char canonical[PATH_MAX]; if (realpath(root.path.fileSystemRepresentation,canonical)) root=[NSURL fileURLWithFileSystemRepresentation:canonical isDirectory:YES relativeToURL:nil];
        state=[ReadJSON([root URLByAppendingPathComponent:@"save-sync.json"],nil,NO) mutableCopy] ?: [NSMutableDictionary new];
        if (!ValidFiles(state[@"files"])) [state removeObjectForKey:@"files"];
        if (device) state[@"device"]=device;
        if (!SafeID(state[@"device"])) state[@"device"]=NSUUID.UUID.UUIDString;
        queue=dispatch_queue_create("com.dmikey.cara.save-sync",DISPATCH_QUEUE_SERIAL);
        _status=[self enabled]?@"Save folder connected":@"Not connected";
    }
    return self;
}
- (BOOL)enabled { @synchronized(self) { return directFolder!=nil || state[@"bookmark"]!=nil; } }
- (void)saveState { WriteJSON(state,[root URLByAppendingPathComponent:@"save-sync.json"],nil,NO); }
- (BOOL)connectFolder:(NSURL*)url error:(NSError**)error {
    BOOL scoped=[url startAccessingSecurityScopedResource];
    NSData* bookmark=[url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark includingResourceValuesForKeys:nil relativeToURL:nil error:error];
    if (scoped) [url stopAccessingSecurityScopedResource]; if (!bookmark) return NO;
    @synchronized(self) { directFolder=nil; state[@"bookmark"]=[bookmark base64EncodedStringWithOptions:0]; [state removeObjectForKey:@"files"]; [state removeObjectForKey:@"base"]; [self saveState]; self.status=@"Connected; saves sync after saving or stopping"; }
    return YES;
}
- (void)disconnect { @synchronized(self) { directFolder=nil; [state removeObjectForKey:@"bookmark"]; [self saveState]; self.status=@"Not connected"; } }
- (NSURL*)folder:(NSError**)error {
    @synchronized(self) {
        if (directFolder) return directFolder;
        NSData* data=[[NSData alloc] initWithBase64EncodedString:state[@"bookmark"]?:@"" options:0]; BOOL stale=NO;
        NSURL* url=[NSURL URLByResolvingBookmarkData:data options:NSURLBookmarkResolutionWithoutUI relativeToURL:nil bookmarkDataIsStale:&stale error:error];
        if (stale && url) { BOOL scope=[url startAccessingSecurityScopedResource]; NSData* fresh=[url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark includingResourceValuesForKeys:nil relativeToURL:nil error:nil]; if (fresh) { state[@"bookmark"]=[fresh base64EncodedStringWithOptions:0]; [self saveState]; } if (scope) [url stopAccessingSecurityScopedResource]; }
        return url;
    }
}
- (NSURL*)checkpoint:(NSError**)error {
    NSDictionary* files=Collect(root,error); if (!files) return nil;
    NSURL* stage=[[root URLByAppendingPathComponent:@"SaveCheckpoints" isDirectory:YES] URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
    NSURL* payload=[stage URLByAppendingPathComponent:@"Files" isDirectory:YES]; if (!MakeDirectory(payload,error)) return nil;
    for (NSString* path in files) if (!CopyFile([root URLByAppendingPathComponent:path],[payload URLByAppendingPathComponent:path],files[path][@"hash"],error,NO)) return nil;
    if (!WriteJSON(@{@"files":files},[stage URLByAppendingPathComponent:@"manifest.json"],error,NO)) return nil;
    return stage;
}
- (NSDictionary*)loadSnapshot:(NSString*)identifier store:(NSURL*)store cache:(NSMutableDictionary*)cache error:(NSError**)error {
    if (!SafeID(identifier)) { if (error) *error=SaveError(@"Invalid cloud version identifier."); return nil; }
    if (cache[identifier]) return cache[identifier];
    NSURL* url=[[[store URLByAppendingPathComponent:@"Snapshots"] URLByAppendingPathComponent:identifier] URLByAppendingPathComponent:@"manifest.json"];
    NSDictionary* value=ReadJSON(url,error,YES);
    if (!value || ![value[@"id"] isEqual:identifier] || !ValidFiles(value[@"files"]) || ![value[@"parents"] isKindOfClass:NSArray.class] || [value[@"parents"] count]>64) { if (error && !*error) *error=SaveError(@"Invalid cloud save manifest."); return nil; }
    for (NSString* parent in value[@"parents"]) if (!SafeID(parent)) { if (error) *error=SaveError(@"Invalid cloud ancestry."); return nil; }
    cache[identifier]=value; return value;
}
- (BOOL)ancestor:(NSString*)target of:(NSString*)identifier store:(NSURL*)store cache:(NSMutableDictionary*)cache seen:(NSMutableSet*)seen {
    if (seen.count>=128 || [seen containsObject:identifier]) return NO; [seen addObject:identifier];
    NSDictionary* value=[self loadSnapshot:identifier store:store cache:cache error:nil];
    for (NSString* parent in value[@"parents"]) if ([parent isEqual:target] || [self ancestor:target of:parent store:store cache:cache seen:seen]) return YES;
    return NO;
}
- (NSDictionary*)syncCheckpoint:(NSURL*)checkpoint restore:(BOOL)restore resolution:(NSString*)resolution error:(NSError**)error {
    @synchronized(self) {
    NSURL* selected=[self folder:error]; if (!selected) return nil;
    BOOL scoped=[selected startAccessingSecurityScopedResource];
    @try {
        NSURL* store=[selected URLByAppendingPathComponent:@"Cara Saves" isDirectory:YES];
        NSURL* heads=[store URLByAppendingPathComponent:@"Heads" isDirectory:YES];
        NSURL* snapshots=[store URLByAppendingPathComponent:@"Snapshots" isDirectory:YES];
        if (!MakeDirectory(heads,error) || !MakeDirectory(snapshots,error)) return nil;
        NSDictionary* local=ReadJSON([checkpoint URLByAppendingPathComponent:@"manifest.json"],error,NO)[@"files"]; if (!ValidFiles(local)) return nil;
        NSMutableDictionary* cache=[NSMutableDictionary new]; NSMutableArray* ids=[NSMutableArray new];
        NSArray* entries=[NSFileManager.defaultManager contentsOfDirectoryAtURL:heads includingPropertiesForKeys:nil options:NSDirectoryEnumerationSkipsHiddenFiles error:error];
        if (!entries || entries.count>64) return nil;
        for (NSURL* head in entries) {
            if (![head.pathExtension isEqual:@"json"]) continue;
            NSDictionary* pointer=ReadJSON(head,error,YES); NSString* identifier=pointer[@"snapshot"];
            if (!pointer || ![self loadSnapshot:identifier store:store cache:cache error:error]) return nil;
            if (![ids containsObject:identifier]) [ids addObject:identifier];
        }
        NSMutableArray* tips=[ids mutableCopy];
        for (NSString* a in ids) for (NSString* b in ids) if (![a isEqual:b] && [self ancestor:a of:b store:store cache:cache seen:[NSMutableSet new]]) { [tips removeObject:a]; break; }
        NSDictionary* base; NSString* device;
        @synchronized(self) { base=[state[@"files"] copy]?:@{}; device=state[@"device"]; }
        if (!ids.count && base.count && ![resolution isEqual:@"local"]) { if (error) *error=SaveError(@"Cloud history is unavailable. Local saves were kept."); return nil; }
        NSMutableArray* versions=[NSMutableArray new];
        for (NSString* identifier in tips) [versions addObject:@{@"id":identifier,@"created":cache[identifier][@"created"]?:@0}];
        NSString* chosen=tips.firstObject;
        BOOL explicitRemote=resolution.length && ![resolution isEqual:@"local"];
        if (explicitRemote) { if (![tips containsObject:resolution]) { if (error) *error=SaveError(@"That cloud version changed. Please try again."); return nil; } chosen=resolution; }
        if (tips.count>1 && !resolution.length) {
            BOOL equal=YES; for (NSString* identifier in tips) if (![cache[identifier][@"files"] isEqual:cache[chosen][@"files"]]) equal=NO;
            if (!equal) return @{@"conflict":@YES,@"versions":versions};
        }
        NSDictionary* remote=chosen?cache[chosen][@"files"]:@{};
        if (!restore && ![remote isEqual:base] && ![remote isEqual:local]) return @{@"conflict":@YES,@"versions":versions,@"requiresStop":@YES};
        NSMutableSet* paths=[NSMutableSet setWithArray:local.allKeys]; [paths addObjectsFromArray:remote.allKeys]; [paths addObjectsFromArray:base.allKeys];
        NSMutableDictionary* merged=[NSMutableDictionary new]; NSMutableSet* remotePaths=[NSMutableSet new]; BOOL conflict=NO;
        for (NSString* path in paths) {
            NSString* l=local[path][@"hash"],*r=remote[path][@"hash"],*b=base[path][@"hash"];
            const auto equal=[](id x,id y) { return x==y || [x isEqual:y]; };
            BOOL takeRemote=NO;
            if ([resolution isEqual:@"local"]) takeRemote=NO;
            else if (explicitRemote) takeRemote=YES;
            else if (equal(l,r)) takeRemote=NO;
            else if (equal(l,b)) takeRemote=YES;
            else if (equal(r,b)) takeRemote=NO;
            else { conflict=YES; continue; }
            NSDictionary* value=takeRemote?remote[path]:local[path];
            if (value) { merged[path]=value; if (takeRemote) [remotePaths addObject:path]; }
        }
        if (conflict) return @{@"conflict":@YES,@"versions":versions};
        NSURL* final=[[root URLByAppendingPathComponent:@"SaveCheckpoints"] URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
        NSURL* payload=[final URLByAppendingPathComponent:@"Files"];
        if (!MakeDirectory(payload,error)) return nil;
        for (NSString* path in merged) {
            NSURL* source;
            BOOL cloud=[remotePaths containsObject:path];
            if (cloud && !NoSaveLinks(snapshots,[[chosen stringByAppendingPathComponent:@"Files"] stringByAppendingPathComponent:path],error)) return nil;
            if (cloud) source=[[[[snapshots URLByAppendingPathComponent:chosen] URLByAppendingPathComponent:@"Files"] URLByAppendingPathComponent:path] standardizedURL];
            else source=[[checkpoint URLByAppendingPathComponent:@"Files"] URLByAppendingPathComponent:path];
            if (!CopyFile(source,[payload URLByAppendingPathComponent:path],merged[path][@"hash"],error,cloud)) return nil;
        }
        if (restore && ![merged isEqual:local]) {
            // Preserve the complete local checkpoint before any replacement/deletion.
            NSURL* backup=[[root URLByAppendingPathComponent:@"SaveBackups"] URLByAppendingPathComponent:NSUUID.UUID.UUIDString];
            if (!MakeDirectory(backup.URLByDeletingLastPathComponent,error) || ![NSFileManager.defaultManager copyItemAtURL:checkpoint toURL:backup error:error]) return nil;
            NSMutableSet* touched=[NSMutableSet setWithArray:local.allKeys]; [touched addObjectsFromArray:merged.allKeys];
            for (NSString* path in touched) if (!NoSaveLinks(root,path,error)) return nil;
            BOOL applied=YES;
            for (NSString* path in touched) {
                NSURL* destination=[root URLByAppendingPathComponent:path];
                if (merged[path]) {
                    NSData* data=[NSData dataWithContentsOfURL:[payload URLByAppendingPathComponent:path] options:0 error:error];
                    if (!data || !MakeDirectory(destination.URLByDeletingLastPathComponent,error) || ![data writeToURL:destination options:NSDataWritingAtomic error:error]) { applied=NO; break; }
                } else if ([NSFileManager.defaultManager fileExistsAtPath:destination.path] && ![NSFileManager.defaultManager removeItemAtURL:destination error:error]) { applied=NO; break; }
            }
            if (!applied) {
                // Roll back only managed save paths; unrelated app data is untouched.
                for (NSString* path in touched) {
                    NSURL* destination=[root URLByAppendingPathComponent:path];
                    [NSFileManager.defaultManager removeItemAtURL:destination error:nil];
                    if (local[path]) { MakeDirectory(destination.URLByDeletingLastPathComponent,nil); [NSFileManager.defaultManager copyItemAtURL:[[backup URLByAppendingPathComponent:@"Files"] URLByAppendingPathComponent:path] toURL:destination error:nil]; }
                }
                return nil;
            }
        }
        NSString* published=chosen;
        if (![merged isEqual:remote] || !chosen || tips.count>1) {
            published=NSUUID.UUID.UUIDString;
            NSDictionary* manifest=@{@"id":published,@"device":device,@"created":@(NSDate.date.timeIntervalSince1970),@"parents":ids,@"files":merged};
            if (!WriteJSON(manifest,[final URLByAppendingPathComponent:@"manifest.json"],error,NO)) return nil;
            __block NSError* failure=nil; __block BOOL copied=NO;
            [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateWritingItemAtURL:snapshots options:NSFileCoordinatorWritingForMerging error:&failure byAccessor:^(NSURL* actual) { copied=[NSFileManager.defaultManager copyItemAtURL:final toURL:[actual URLByAppendingPathComponent:published isDirectory:YES] error:&failure]; }];
            if (!copied) { if (error) *error=failure; return nil; }
            if (!WriteJSON(@{@"snapshot":published},[heads URLByAppendingPathComponent:[device stringByAppendingString:@".json"]],error,YES)) return nil;
        }
        @synchronized(self) { state[@"files"]=merged; if (published) state[@"base"]=published; [self saveState]; }
        [NSFileManager.defaultManager removeItemAtURL:final error:nil];
        self.status=@"Save folder updated; iCloud Drive handles upload";
        return @{@"conflict":@NO,@"files":@(merged.count),@"snapshot":published?:@"",@"folderWriteComplete":@YES,@"cloudUploadVerified":@NO};
    } @finally { if (scoped) [selected stopAccessingSecurityScopedResource]; }
    }
}
- (NSDictionary*)syncStoppedWithResolution:(NSString*)resolution error:(NSError**)error {
    if (!self.enabled) return @{@"disabled":@YES};
    NSURL* checkpoint=[self checkpoint:error]; if (!checkpoint) return nil;
    NSDictionary* result=[self syncCheckpoint:checkpoint restore:YES resolution:resolution error:error];
    [NSFileManager.defaultManager removeItemAtURL:checkpoint error:nil]; return result;
}
- (void)requestSync:(BOOL)allowRestore resolution:(NSString*)resolution completion:(void(^)(NSDictionary*,NSError*))completion {
    dispatch_async(queue,^{ @autoreleasepool {
        NSError* error=nil;
        NSDictionary* result=allowRestore?[self syncStoppedWithResolution:resolution error:&error]:nil;
        if (error) self.status=error.localizedDescription;
        else if ([result[@"conflict"] boolValue]) self.status=@"Save conflict; stop the game and choose a version";
        if (completion) dispatch_async(dispatch_get_main_queue(),^{ completion(result,error); });
    } });
}
- (void)captureCommittedSave {
    if (!self.enabled) return;
    NSError* error=nil; NSURL* checkpoint=[self checkpoint:&error];
    if (!checkpoint) { self.status=error.localizedDescription; return; }
    dispatch_async(queue,^{ @autoreleasepool {
        NSError* failure=nil; NSDictionary* result=[self syncCheckpoint:checkpoint restore:NO resolution:nil error:&failure];
        if (failure) self.status=failure.localizedDescription;
        else if ([result[@"conflict"] boolValue]) self.status=@"Remote saves changed; stop the game to sync";
        [NSFileManager.defaultManager removeItemAtURL:checkpoint error:nil];
    } });
}
@end
