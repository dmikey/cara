// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/save_sync.h"
#import <CommonCrypto/CommonDigest.h>
#include <cstdlib>
#include <limits.h>
#include <atomic>
#include <cstdio>

NSNotificationName const CaraSaveSyncDidChangeNotification = @"CaraSaveSyncDidChange";

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
BOOL CaraIsStatePath(NSString* relative) {
    if (![relative isKindOfClass:NSString.class]) return NO;
    static NSRegularExpression* expression;
    static dispatch_once_t once;
    dispatch_once(&once,^{ expression=[NSRegularExpression regularExpressionWithPattern:@"^states/(?:(?:[0-9A-F]{16}|homebrew-[0-9A-F]{64})\\.(?:0[1-9]|10|quick|(?:manual|auto|backup)-[0-9A-F-]{36})\\.carastate|metadata\\.json)$" options:0 error:nil]; });
    return relative.length<180 && NSEqualRanges([expression rangeOfFirstMatchInString:relative options:0 range:NSMakeRange(0,relative.length)],NSMakeRange(0,relative.length));
}
BOOL CaraIsConfigurationPath(NSString* relative) {
    if (![relative isKindOfClass:NSString.class] || relative.length>200) return NO;
    if ([@[@"settings/controller-preferences.json",@"settings/skins/portrait.deltaskin",@"settings/skins/landscape.deltaskin"] containsObject:relative]) return YES;
    static NSRegularExpression* cheats; static dispatch_once_t once;
    dispatch_once(&once,^{ cheats=[NSRegularExpression regularExpressionWithPattern:@"^cheats/[0-9A-F]{16}\\.txt$" options:0 error:nil]; });
    return NSEqualRanges([cheats rangeOfFirstMatchInString:relative options:0 range:NSMakeRange(0,relative.length)],NSMakeRange(0,relative.length));
}
static unsigned long long FileLimit(NSString* path) {
    if ([@[@"states/metadata.json",@"settings/controller-preferences.json"] containsObject:path]) return 2ull*1024*1024;
    if (CaraIsConfigurationPath(path)) return [path hasPrefix:@"cheats/"]?1024ull*1024:32ull*1024*1024;
    return CaraIsStatePath(path)?768ull*1024*1024+256:128ull*1024*1024;
}
static unsigned long long SetLimit(CaraSyncContent content) { return content==CaraSyncSaveStates?8ull*1024*1024*1024:content==CaraSyncConfiguration?128ull*1024*1024:512ull*1024*1024; }
static BOOL ManagedPath(NSString* path,CaraSyncContent content) { return content==CaraSyncSaveStates?CaraIsStatePath(path):content==CaraSyncConfiguration?CaraIsConfigurationPath(path):CaraIsSavePath(path); }
static NSString* HashFile(NSURL* url,NSError** error) {
    NSDictionary* values=[url resourceValuesForKeys:@[NSURLIsRegularFileKey,NSURLIsSymbolicLinkKey,NSURLFileSizeKey] error:error];
    if (!values || ![values[NSURLIsRegularFileKey] boolValue] || [values[NSURLIsSymbolicLinkKey] boolValue] || [values[NSURLFileSizeKey] unsignedLongLongValue]>([url.pathExtension isEqual:@"carastate"]?768ull*1024*1024+256:128ull*1024*1024)) { if (error && !*error) *error=SaveError(@"A save file is invalid or too large."); return nil; }
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
static NSDictionary* Collect(NSURL* root,NSError** error,CaraSyncContent content) {
    NSMutableDictionary* files=[NSMutableDictionary new]; unsigned long long total=0;
    for (NSString* sub in (content==CaraSyncSaveStates?@[@"states"]:content==CaraSyncConfiguration?@[@"settings",@"cheats"]:@[@"sdmc/Nintendo 3DS",@"nand/data"])) {
        NSURL* base=[root URLByAppendingPathComponent:sub isDirectory:YES];
        if (![NSFileManager.defaultManager fileExistsAtPath:base.path]) continue;
        __block NSError* enumerationError=nil;
        NSDirectoryEnumerator* e=[NSFileManager.defaultManager enumeratorAtURL:base includingPropertiesForKeys:@[NSURLIsRegularFileKey,NSURLIsSymbolicLinkKey,NSURLFileSizeKey] options:0 errorHandler:^BOOL(NSURL* url,NSError* failure) { enumerationError=failure; return NO; }];
        for (NSURL* url in e) {
            if ([url.lastPathComponent isEqual:@"content"]) { [e skipDescendants]; continue; }
            NSString* rootPath=root.path;
            NSString* prefix=[rootPath hasSuffix:@"/"]?rootPath:[rootPath stringByAppendingString:@"/"];
            if (![url.path hasPrefix:prefix]) continue;
            NSString* path=[url.path substringFromIndex:prefix.length]; if (!ManagedPath(path,content)) continue;
            NSDictionary* info=[url resourceValuesForKeys:@[NSURLIsRegularFileKey,NSURLIsSymbolicLinkKey,NSURLFileSizeKey] error:error];
            if ([info[NSURLIsSymbolicLinkKey] boolValue]) { if (error) *error=SaveError(@"Save links are not supported."); return nil; }
            if (![info[NSURLIsRegularFileKey] boolValue]) continue;
            total+=[info[NSURLFileSizeKey] unsignedLongLongValue];
            if ([info[NSURLFileSizeKey] unsignedLongLongValue]>FileLimit(path) || total>SetLimit(content) || files.count>=4096) { if (error) *error=SaveError(@"The save set is too large."); return nil; }
            NSString* hash=HashFile(url,error); if (!hash) return nil;
            files[path]=@{@"hash":hash,@"size":info[NSURLFileSizeKey]};
        }
        if (enumerationError) { if (error) *error=enumerationError; return nil; }
        if (error && *error) return nil;
    }
    return files;
}
static BOOL ValidFiles(NSDictionary* files,CaraSyncContent content) {
    if (![files isKindOfClass:NSDictionary.class] || files.count>4096) return NO;
    unsigned long long total=0;
    for (NSString* path in files) {
        NSDictionary* entry=files[path]; if (!ManagedPath(path,content) || ![entry isKindOfClass:NSDictionary.class]) return NO;
        NSString* hash=entry[@"hash"];
        if (![hash isKindOfClass:NSString.class] || hash.length!=64 || [hash rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location!=NSNotFound || ![entry[@"size"] isKindOfClass:NSNumber.class]) return NO;
        const unsigned long long size=[entry[@"size"] unsignedLongLongValue]; total+=size;
        if (size>FileLimit(path) || total>SetLimit(content)) return NO;
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
static BOOL CopyFile(NSURL* source,NSURL* destination,NSString* hash,NSNumber* expectedSize,NSError** error,BOOL coordinated) {
    if (!MakeDirectory(destination.URLByDeletingLastPathComponent,error)) return NO;
    __block BOOL ok=NO; __block NSError* failure=nil;
    void (^copy)(NSURL*)=^(NSURL* actual) {
        NSString* actualHash=HashFile(actual,&failure);
        if (![actualHash isEqual:hash]) { if (!failure) failure=SaveError(@"A save changed or its checksum did not match. Local saves were kept."); return; }
        NSNumber* sourceSize=nil; [actual getResourceValue:&sourceSize forKey:NSURLFileSizeKey error:&failure];
        if (!sourceSize || sourceSize.unsignedLongLongValue!=expectedSize.unsignedLongLongValue) { failure=SaveError(@"A save size did not match its manifest. Local data was kept."); return; }
        ok=[NSFileManager.defaultManager copyItemAtURL:actual toURL:destination error:&failure];
        if (ok && ![HashFile(destination,&failure) isEqual:hash]) { ok=NO; if (!failure) failure=SaveError(@"A save changed while copying. Local data was kept."); }
        if (!ok) [NSFileManager.defaultManager removeItemAtURL:destination error:nil];
    };
    if (coordinated) [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateReadingItemAtURL:source options:0 error:&failure byAccessor:copy]; else copy(source);
    if (!ok && error) *error=failure?:SaveError(@"Could not copy save data."); return ok;
}

@interface CaraSaveSync ()
@property(atomic,copy,readwrite) NSString* status;
@property(atomic,copy,readwrite) NSString* detail;
@property(atomic,copy,readwrite) NSString* phase;
@property(atomic,readwrite) BOOL enabled;
@property(atomic,readwrite) BOOL cloudUploadVerified;
@property(atomic,strong,readwrite) NSDate* lastFolderWrite;
@end
@implementation CaraSaveSync {
    NSURL* root;
    NSURL* directFolder;
    NSMutableDictionary* state;
    dispatch_queue_t queue;
    std::atomic<unsigned> pending;
    NSArray<NSURL*>* uploadFiles;
    NSString* metadataName;
    NSString* checkpointDirectory;
    NSString* backupDirectory;
    NSString* storeName;
}
@synthesize status=_status;
- (instancetype)initWithRoot:(NSURL*)url { return [self initWithRoot:url folder:nil device:nil content:CaraSyncGameSaves]; }
- (instancetype)initWithRoot:(NSURL*)url statesOnly:(BOOL)onlyStates { return [self initWithRoot:url folder:nil device:nil content:onlyStates?CaraSyncSaveStates:CaraSyncGameSaves]; }
- (instancetype)initWithRoot:(NSURL*)url folder:(NSURL*)folder device:(NSString*)device { return [self initWithRoot:url folder:folder device:device content:CaraSyncGameSaves]; }
- (instancetype)initWithRoot:(NSURL*)url folder:(NSURL*)folder device:(NSString*)device statesOnly:(BOOL)onlyStates { return [self initWithRoot:url folder:folder device:device content:onlyStates?CaraSyncSaveStates:CaraSyncGameSaves]; }
- (instancetype)initWithRoot:(NSURL*)url content:(CaraSyncContent)content { return [self initWithRoot:url folder:nil device:nil content:content]; }
- (instancetype)initWithRoot:(NSURL*)url folder:(NSURL*)folder device:(NSString*)device content:(CaraSyncContent)content {
    const BOOL onlyStates=content==CaraSyncSaveStates;
    const BOOL configuration=content==CaraSyncConfiguration;
    if ((self=[super init])) {
        _content=content; _statesOnly=onlyStates; _configurationOnly=configuration; pending=0;
        metadataName=configuration?@"configuration-sync.json":onlyStates?@"state-sync.json":@"save-sync.json";
        checkpointDirectory=configuration?@"ConfigurationCheckpoints":onlyStates?@"StateCheckpoints":@"SaveCheckpoints";
        backupDirectory=configuration?@"ConfigurationBackups":onlyStates?@"StateBackups":@"SaveBackups";
        storeName=configuration?@"Cara Settings":onlyStates?@"Cara States":@"Cara Saves";
        root=url; directFolder=folder; MakeDirectory(root,nil);
        char canonical[PATH_MAX]; if (realpath(root.path.fileSystemRepresentation,canonical)) root=[NSURL fileURLWithFileSystemRepresentation:canonical isDirectory:YES relativeToURL:nil];
        NSDictionary* previous=ReadJSON([root URLByAppendingPathComponent:metadataName],nil,NO);
        state=[previous mutableCopy] ?: [NSMutableDictionary new];
        // Upgrade existing folder authorization once; an explicit disconnect
        // in the new channel must stay disconnected on later launches.
        if (content!=CaraSyncGameSaves && !previous) {
            NSString* bookmark=ReadJSON([root URLByAppendingPathComponent:@"save-sync.json"],nil,NO)[@"bookmark"];
            if (bookmark) state[@"bookmark"]=bookmark;
        }
        if (!ValidFiles(state[@"files"],_content)) [state removeObjectForKey:@"files"];
        if (device) state[@"device"]=device;
        if (!SafeID(state[@"device"])) state[@"device"]=NSUUID.UUID.UUIDString;
        queue=dispatch_queue_create(onlyStates?"com.dmikey.cara.state-sync":"com.dmikey.cara.save-sync",DISPATCH_QUEUE_SERIAL);
        WriteJSON(state,[root URLByAppendingPathComponent:metadataName],nil,NO);
        _enabled=directFolder!=nil || state[@"bookmark"]!=nil;
        _phase=_enabled?@"connected":@"disconnected";
        _status=_enabled?@"Folder connected":@"Not connected";
        _detail=_enabled?@"Ready to check for changes.":@"Choose a folder in iCloud Drive to sync across devices.";
        if ([state[@"lastFolderWrite"] doubleValue]>0) _lastFolderWrite=[NSDate dateWithTimeIntervalSince1970:[state[@"lastFolderWrite"] doubleValue]];
    }
    return self;
}
- (NSString*)contentName { return self.configurationOnly?@"Controls & cheats":self.statesOnly?@"Save states":@"In-game saves"; }
- (void)announce {
    dispatch_async(dispatch_get_main_queue(),^{ [NSNotificationCenter.defaultCenter postNotificationName:CaraSaveSyncDidChangeNotification object:self]; });
}
- (void)phase:(NSString*)value title:(NSString*)title detail:(NSString*)detail {
    self.phase=value; self.status=title; self.detail=detail?:@""; [self announce];
}
- (BOOL)busy { return pending.load(std::memory_order_acquire)>0; }
- (void)beginSync {
    ++pending;
    [self phase:@"preparing" title:(self.configurationOnly?@"Preparing controls & cheats…":self.statesOnly?@"Preparing save states…":@"Preparing saves…") detail:@"Your local data stays available while the folder is updated."];
}
- (void)finish:(NSDictionary*)result error:(NSError*)error {
    if (error) [self phase:@"error" title:@"Couldn’t sync" detail:error.localizedDescription];
    else if ([result[@"conflict"] boolValue]) [self phase:@"conflict" title:@"Choose a version" detail:@"Both versions are preserved. Stop the game, then resolve this in Settings."];
    --pending; [self announce];
}
- (void)saveState { WriteJSON(state,[root URLByAppendingPathComponent:metadataName],nil,NO); }
- (BOOL)connectFolder:(NSURL*)url error:(NSError**)error {
    if (self.busy) { if (error) *error=SaveError(@"Wait for the current sync to finish before changing folders."); return NO; }
    BOOL scoped=[url startAccessingSecurityScopedResource];
    NSData* bookmark=[url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark includingResourceValuesForKeys:nil relativeToURL:nil error:error];
    if (scoped) [url stopAccessingSecurityScopedResource]; if (!bookmark) return NO;
    @synchronized(self) { directFolder=nil; state[@"bookmark"]=[bookmark base64EncodedStringWithOptions:0]; [state removeObjectForKey:@"files"]; [state removeObjectForKey:@"base"]; [self saveState]; self.enabled=YES; self.cloudUploadVerified=NO; uploadFiles=nil; [self phase:@"connected" title:@"Folder connected" detail:@"Changes sync after saving and before opening a game."]; }
    return YES;
}
- (void)disconnect { if (self.busy) return; @synchronized(self) { directFolder=nil; [state removeObjectForKey:@"bookmark"]; [self saveState]; self.enabled=NO; self.cloudUploadVerified=NO; uploadFiles=nil; [self phase:@"disconnected" title:@"Not connected" detail:@"Your local saves and states are kept on this device."]; } }
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
    NSDictionary* files=Collect(root,error,self.content); if (!files) return nil;
    NSURL* stage=[[root URLByAppendingPathComponent:checkpointDirectory isDirectory:YES] URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
    BOOL complete=NO;
    @try {
        NSURL* payload=[stage URLByAppendingPathComponent:@"Files" isDirectory:YES]; if (!MakeDirectory(payload,error)) return nil;
        for (NSString* path in files) if (!CopyFile([root URLByAppendingPathComponent:path],[payload URLByAppendingPathComponent:path],files[path][@"hash"],files[path][@"size"],error,NO)) return nil;
        if (!WriteJSON(@{@"files":files},[stage URLByAppendingPathComponent:@"manifest.json"],error,NO)) return nil;
        complete=YES; return stage;
    } @finally { if (!complete) [NSFileManager.defaultManager removeItemAtURL:stage error:nil]; }

}
- (NSDictionary*)loadSnapshot:(NSString*)identifier store:(NSURL*)store cache:(NSMutableDictionary*)cache error:(NSError**)error {
    if (!SafeID(identifier)) { if (error) *error=SaveError(@"Invalid cloud version identifier."); return nil; }
    if (cache[identifier]) return cache[identifier];
    NSURL* url=[[[store URLByAppendingPathComponent:@"Snapshots"] URLByAppendingPathComponent:identifier] URLByAppendingPathComponent:@"manifest.json"];
    NSDictionary* value=ReadJSON(url,error,YES);
    if (!value || ![value[@"id"] isEqual:identifier] || !ValidFiles(value[@"files"],self.content) || ![value[@"parents"] isKindOfClass:NSArray.class] || [value[@"parents"] count]>64) { if (error && !*error) *error=SaveError(@"Invalid cloud save manifest."); return nil; }
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
    NSURL* temporaryMerge=nil;
    @try {
        NSURL* store=[selected URLByAppendingPathComponent:storeName isDirectory:YES];
        NSURL* heads=[store URLByAppendingPathComponent:@"Heads" isDirectory:YES];
        NSURL* snapshots=[store URLByAppendingPathComponent:@"Snapshots" isDirectory:YES];
        if (!MakeDirectory(heads,error) || !MakeDirectory(snapshots,error)) return nil;
        NSDictionary* local=ReadJSON([checkpoint URLByAppendingPathComponent:@"manifest.json"],error,NO)[@"files"]; if (!ValidFiles(local,self.content)) return nil;
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
        NSURL* final=[[root URLByAppendingPathComponent:checkpointDirectory] URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
        temporaryMerge=final;
        NSURL* payload=[final URLByAppendingPathComponent:@"Files"];
        if (!MakeDirectory(payload,error)) return nil;
        for (NSString* path in merged) {
            NSURL* source;
            BOOL cloud=[remotePaths containsObject:path];
            if (cloud && !NoSaveLinks(snapshots,[[chosen stringByAppendingPathComponent:@"Files"] stringByAppendingPathComponent:path],error)) return nil;
            if (cloud) source=[[[[snapshots URLByAppendingPathComponent:chosen] URLByAppendingPathComponent:@"Files"] URLByAppendingPathComponent:path] standardizedURL];
            else source=[[checkpoint URLByAppendingPathComponent:@"Files"] URLByAppendingPathComponent:path];
            if (!CopyFile(source,[payload URLByAppendingPathComponent:path],merged[path][@"hash"],merged[path][@"size"],error,cloud)) return nil;
        }
        if (restore && ![merged isEqual:local]) {
            // Preserve the complete local checkpoint before any replacement/deletion.
            NSURL* backup=[[root URLByAppendingPathComponent:backupDirectory] URLByAppendingPathComponent:NSUUID.UUID.UUIDString];
            if (!MakeDirectory(backup.URLByDeletingLastPathComponent,error) || ![NSFileManager.defaultManager copyItemAtURL:checkpoint toURL:backup error:error]) return nil;
            NSMutableSet* touched=[NSMutableSet setWithArray:local.allKeys]; [touched addObjectsFromArray:merged.allKeys];
            for (NSString* path in touched) if (!NoSaveLinks(root,path,error)) return nil;
            BOOL applied=YES;
            for (NSString* path in touched) {
                NSURL* destination=[root URLByAppendingPathComponent:path];
                if (merged[path]) {
                    // Keep large states off the heap and commit each replacement atomically.
                    NSURL* temporary=[destination.URLByDeletingLastPathComponent URLByAppendingPathComponent:[@".cara-sync-" stringByAppendingString:NSUUID.UUID.UUIDString]];
                    if (!MakeDirectory(destination.URLByDeletingLastPathComponent,error) ||
                        ![NSFileManager.defaultManager copyItemAtURL:[payload URLByAppendingPathComponent:path] toURL:temporary error:error]) { applied=NO; break; }
                    if (std::rename(temporary.path.fileSystemRepresentation,destination.path.fileSystemRepresentation)!=0) {
                        [NSFileManager.defaultManager removeItemAtURL:temporary error:nil];
                        if (error) *error=SaveError(@"Could not replace a save. The previous local version was kept.");
                        applied=NO; break;
                    }
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
        @synchronized(self) { state[@"files"]=merged; if (published) state[@"base"]=published;
            self.lastFolderWrite=NSDate.date; state[@"lastFolderWrite"]=@(self.lastFolderWrite.timeIntervalSince1970); [self saveState]; }
        [NSFileManager.defaultManager removeItemAtURL:final error:nil];
        NSMutableArray* tracked=[NSMutableArray new];
        NSURL* ownHead=[heads URLByAppendingPathComponent:[device stringByAppendingString:@".json"]];
        if ([NSFileManager.defaultManager fileExistsAtPath:ownHead.path]) [tracked addObject:ownHead];
        if (published) {
            NSURL* snapshot=[snapshots URLByAppendingPathComponent:published];
            [tracked addObject:[snapshot URLByAppendingPathComponent:@"manifest.json"]];
            for (NSString* path in merged) [tracked addObject:[[snapshot URLByAppendingPathComponent:@"Files"] URLByAppendingPathComponent:path]];
        }
        uploadFiles=tracked; self.cloudUploadVerified=NO;
        [self updateUploadStatus];
        return @{@"conflict":@NO,@"files":@(merged.count),@"snapshot":published?:@"",@"folderWriteComplete":@YES,@"cloudUploadVerified":@NO};
    } @finally { if (temporaryMerge) [NSFileManager.defaultManager removeItemAtURL:temporaryMerge error:nil]; if (scoped) [selected stopAccessingSecurityScopedResource]; }
    }
}
- (void)updateUploadStatus {
    if (!uploadFiles.count) return;
    NSUInteger uploaded=0, cloudItems=0;
    for (NSURL* file in uploadFiles) {
        NSDictionary* values=[file resourceValuesForKeys:@[NSURLIsUbiquitousItemKey,NSURLUbiquitousItemIsUploadedKey,NSURLUbiquitousItemUploadingErrorKey] error:nil];
        if ([values[NSURLIsUbiquitousItemKey] boolValue]) {
            ++cloudItems;
            NSError* error=values[NSURLUbiquitousItemUploadingErrorKey];
            if ([error isKindOfClass:NSError.class]) { [self phase:@"error" title:@"iCloud upload needs attention" detail:error.localizedDescription]; return; }
            if ([values[NSURLUbiquitousItemIsUploadedKey] boolValue]) ++uploaded;
        }
    }
    if (cloudItems==uploadFiles.count && uploaded==cloudItems) {
        self.cloudUploadVerified=YES;
        [self phase:@"uploaded" title:@"Uploaded to iCloud" detail:@"iCloud reports these files are uploaded. Other devices check for changes before opening a game."];
    } else if (cloudItems) {
        self.cloudUploadVerified=NO;
        [self phase:@"pendingUpload" title:@"Waiting for iCloud" detail:[NSString stringWithFormat:@"%lu of %lu files uploaded. iCloud Drive continues in the background.",(unsigned long)uploaded,(unsigned long)uploadFiles.count]];
    } else {
        self.cloudUploadVerified=NO;
        [self phase:@"folderReady" title:@"Folder updated" detail:@"Your selected folder is up to date. Its file provider manages upload to other devices."];
    }
}
- (void)refreshCloudStatus {
    if (!self.enabled || self.busy) return;
    dispatch_async(queue,^{ @autoreleasepool {
        if ([self.phase isEqual:@"conflict"] || [self.phase isEqual:@"error"]) return;
        NSError* error=nil; NSURL* selected=[self folder:&error];
        if (!selected) { [self phase:@"error" title:@"Folder unavailable" detail:error.localizedDescription?:@"Choose your sync folder again in Settings."]; return; }
        BOOL scoped=[selected startAccessingSecurityScopedResource];
        [self updateUploadStatus];
        if (scoped) [selected stopAccessingSecurityScopedResource];
    } });
}
- (NSDictionary*)syncStoppedWithResolution:(NSString*)resolution error:(NSError**)error {
    if (!self.enabled) return @{@"disabled":@YES};
    [self beginSync];
    NSError* failure=nil;
    NSURL* checkpoint=[self checkpoint:&failure];
    NSDictionary* result=checkpoint?[self syncCheckpoint:checkpoint restore:YES resolution:resolution error:&failure]:nil;
    if (checkpoint) [NSFileManager.defaultManager removeItemAtURL:checkpoint error:nil];
    if (!result && !failure) failure=SaveError(@"The sync folder could not be updated. Local data was kept.");
    [self finish:result error:failure]; if (error) *error=failure; return result;
}
- (void)requestSync:(BOOL)allowRestore resolution:(NSString*)resolution completion:(void(^)(NSDictionary*,NSError*))completion {
    if (!self.enabled) { if (completion) dispatch_async(dispatch_get_main_queue(),^{ completion(@{@"disabled":@YES},nil); }); return; }
    [self beginSync];
    dispatch_async(queue,^{ @autoreleasepool {
        NSError* error=nil;
        NSDictionary* result=allowRestore?[self syncStoppedWithResolution:resolution error:&error]:nil;
        if (!allowRestore) error=SaveError(@"Pause and stop the game before restoring synced data.");
        [self finish:result error:error];
        if (completion) dispatch_async(dispatch_get_main_queue(),^{ completion(result,error); });
    } });
}
- (void)captureCommittedSave {
    if (!self.enabled) return;
    [self beginSync];
    NSError* error=nil; NSURL* checkpoint=[self checkpoint:&error];
    if (!checkpoint) { [self finish:nil error:error?:SaveError(@"Could not prepare a local sync checkpoint.")]; return; }
    dispatch_async(queue,^{ @autoreleasepool {
        [self phase:@"syncing" title:(self.configurationOnly?@"Syncing controls & cheats…":self.statesOnly?@"Syncing save states…":@"Syncing saves…") detail:@"Local data is preserved if the sync folder is unavailable."];
        NSError* failure=nil; NSDictionary* result=[self syncCheckpoint:checkpoint restore:NO resolution:nil error:&failure];
        if (!result && !failure) failure=SaveError(@"The sync folder could not be updated. Local data was kept.");
        [NSFileManager.defaultManager removeItemAtURL:checkpoint error:nil];
        [self finish:result error:failure];
    } });
}
@end
