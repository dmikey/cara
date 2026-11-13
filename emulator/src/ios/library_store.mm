// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/library_store.h"

@implementation CaraLibraryGame
@end

static NSError* LibraryError(NSString* message) {
    return [NSError errorWithDomain:@"CaraLibrary" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];
}

@implementation CaraLibraryStore {
    NSURL* documents;
    NSURL* selectedFolder;
    BOOL folderScoped;
    NSUserDefaults* preferences;
    dispatch_queue_t scanQueue;
    unsigned scanGeneration;
}
@synthesize defaultFolder=_defaultFolder, locationError=_locationError;
- (instancetype)initWithDocuments:(NSURL*)url defaults:(NSUserDefaults*)defaults {
    if ((self=[super init])) {
        documents=url; preferences=defaults;
        _defaultFolder=[documents URLByAppendingPathComponent:@"Roms" isDirectory:YES];
        NSError* failure=nil;
        if (![NSFileManager.defaultManager createDirectoryAtURL:_defaultFolder withIntermediateDirectories:YES attributes:nil error:&failure]) _locationError=failure.localizedDescription;
        [_defaultFolder setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
        scanQueue=dispatch_queue_create("com.dmikey.cara.library",DISPATCH_QUEUE_SERIAL);
        NSData* bookmark=[preferences dataForKey:@"romsFolderBookmark"];
        if (bookmark) {
            BOOL stale=NO;
            selectedFolder=[NSURL URLByResolvingBookmarkData:bookmark options:NSURLBookmarkResolutionWithoutUI relativeToURL:nil bookmarkDataIsStale:&stale error:&failure];
            if (selectedFolder) {
                folderScoped=[selectedFolder startAccessingSecurityScopedResource];
                if (stale) {
                    NSData* fresh=[selectedFolder bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
                    if (fresh) [preferences setObject:fresh forKey:@"romsFolderBookmark"];
                }
            } else _locationError=@"Your ROMs folder is unavailable. Choose it again in Settings. Games in Cara are still available.";
        }
    }
    return self;
}
+ (BOOL)isGameURL:(NSURL*)url {
    return [@[@"3ds",@"cci",@"cxi",@"3dsx",@"elf"] containsObject:url.pathExtension.lowercaseString];
}
- (NSString*)folderName { return selectedFolder.lastPathComponent?:@"Roms"; }
- (BOOL)hasCustomFolder { return [preferences dataForKey:@"romsFolderBookmark"]!=nil; }
- (BOOL)chooseFolder:(NSURL*)url error:(NSError**)error {
    BOOL scoped=[url startAccessingSecurityScopedResource];
    NSNumber* directory=nil;
    BOOL reachable=[url getResourceValue:&directory forKey:NSURLIsDirectoryKey error:error];
    NSData* bookmark=reachable && directory.boolValue ? [url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark includingResourceValuesForKeys:nil relativeToURL:nil error:error] : nil;
    if (!bookmark) {
        if (scoped) [url stopAccessingSecurityScopedResource];
        if (error && !*error) *error=LibraryError(@"Choose an accessible folder in Files.");
        return NO;
    }
    if (folderScoped) [selectedFolder stopAccessingSecurityScopedResource];
    selectedFolder=url; folderScoped=scoped; _locationError=nil;
    [preferences setObject:bookmark forKey:@"romsFolderBookmark"];
    return YES;
}
- (void)useDefaultFolder {
    if (folderScoped) [selectedFolder stopAccessingSecurityScopedResource];
    selectedFolder=nil; folderScoped=NO; _locationError=nil;
    [preferences removeObjectForKey:@"romsFolderBookmark"];
}
- (BOOL)rememberGame:(NSURL*)url error:(NSError**)error {
    if (![CaraLibraryStore isGameURL:url]) { if (error) *error=LibraryError(@"Choose an extracted 3DS game."); return NO; }
    NSMutableArray* hidden=[[preferences arrayForKey:@"libraryHiddenPaths"] mutableCopy]; [hidden removeObject:[self keyForGame:url]]; if (hidden) [preferences setObject:hidden forKey:@"libraryHiddenPaths"];
    BOOL scoped=[url startAccessingSecurityScopedResource];
    NSData* bookmark=[url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark includingResourceValuesForKeys:nil relativeToURL:nil error:error];
    if (scoped) [url stopAccessingSecurityScopedResource];
    if (!bookmark) return NO;
    NSMutableArray* saved=[[preferences arrayForKey:@"libraryGameBookmarks"] mutableCopy]?:[NSMutableArray new];
    // Keep one record per resolved file. No ROM bytes are copied into the library.
    NSIndexSet* existing=[saved indexesOfObjectsPassingTest:^BOOL(NSData* data,NSUInteger i,BOOL* stop) {
        if (![data isKindOfClass:NSData.class]) return YES;
        NSURL* old=[NSURL URLByResolvingBookmarkData:data options:NSURLBookmarkResolutionWithoutUI relativeToURL:nil bookmarkDataIsStale:nil error:nil];
        return !old || [old.URLByStandardizingPath isEqual:url.URLByStandardizingPath];
    }];
    [saved removeObjectsAtIndexes:existing]; [saved insertObject:bookmark atIndex:0];
    [preferences setObject:saved forKey:@"libraryGameBookmarks"];
    return YES;
}
- (NSString*)keyForGame:(NSURL*)url { return url.URLByStandardizingPath.path; }
- (NSDictionary*)metadataForGame:(NSURL*)url { return [preferences dictionaryForKey:@"libraryMetadata"][[self keyForGame:url]]?:@{}; }
- (void)setMetadata:(NSDictionary*)metadata forGame:(NSURL*)url {
    NSMutableDictionary* all=[[preferences dictionaryForKey:@"libraryMetadata"] mutableCopy]?:[NSMutableDictionary new];
    NSMutableDictionary* value=[[self metadataForGame:url] mutableCopy]; [value addEntriesFromDictionary:metadata]; all[[self keyForGame:url]]=value; [preferences setObject:all forKey:@"libraryMetadata"];
}
- (void)setFavorite:(BOOL)value game:(NSURL*)url { [self setMetadata:@{@"favorite":@(value)} forGame:url]; }
- (void)recordPlayed:(NSURL*)url { [self setMetadata:@{@"lastPlayed":NSDate.date} forGame:url]; }
- (void)removeGame:(NSURL*)url {
    NSMutableArray* hidden=[[preferences arrayForKey:@"libraryHiddenPaths"] mutableCopy]?:[NSMutableArray new];
    if (![hidden containsObject:[self keyForGame:url]]) [hidden addObject:[self keyForGame:url]];
    [preferences setObject:hidden forKey:@"libraryHiddenPaths"];
    NSMutableArray* saved=[[preferences arrayForKey:@"libraryGameBookmarks"] mutableCopy]?:[NSMutableArray new];
    [saved removeObjectsAtIndexes:[saved indexesOfObjectsPassingTest:^BOOL(NSData* data,NSUInteger i,BOOL* stop) { NSURL* old=[NSURL URLByResolvingBookmarkData:data options:NSURLBookmarkResolutionWithoutUI relativeToURL:nil bookmarkDataIsStale:nil error:nil]; return [old.URLByStandardizingPath isEqual:url.URLByStandardizingPath]; }]];
    [preferences setObject:saved forKey:@"libraryGameBookmarks"];
}
- (BOOL)deleteGameFile:(NSURL*)url error:(NSError**)error {
    BOOL scoped=[url startAccessingSecurityScopedResource]; __block BOOL ok=NO; __block NSError* failure=nil;
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateWritingItemAtURL:url options:NSFileCoordinatorWritingForDeleting error:&failure byAccessor:^(NSURL* actual) { ok=[NSFileManager.defaultManager removeItemAtURL:actual error:&failure]; }];
    if (scoped) [url stopAccessingSecurityScopedResource]; if (ok) [self removeGame:url]; else if(error) *error=failure; return ok;
}
- (void)scan:(void(^)(NSArray<CaraLibraryGame*>*,NSString*))completion {
    const unsigned generation=++scanGeneration;
    NSURL* custom=selectedFolder;
    NSString* initialError=_locationError;
    NSArray* hidden=[[preferences arrayForKey:@"libraryHiddenPaths"] copy]?:@[];
    NSArray* imported=[[preferences arrayForKey:@"libraryGameBookmarks"] copy]?:@[];
    // Take an independent lease so changing locations cannot revoke an in-flight scan.
    BOOL scoped=[custom startAccessingSecurityScopedResource];
    dispatch_async(scanQueue,^{ @autoreleasepool {
        NSMutableDictionary<NSString*,CaraLibraryGame*>* games=[NSMutableDictionary new];
        __block NSString* failure=initialError;
        NSArray* keys=@[NSURLIsRegularFileKey,NSURLIsDirectoryKey,NSURLIsSymbolicLinkKey,NSURLFileSizeKey,NSURLContentModificationDateKey];
        void (^add)(NSURL*,NSString*)=^(NSURL* file,NSString* source) {
            if (![CaraLibraryStore isGameURL:file]) return;
            NSDictionary* values=[file resourceValuesForKeys:keys error:nil];
            if (![values[NSURLIsRegularFileKey] boolValue] || [values[NSURLIsSymbolicLinkKey] boolValue]) return;
            NSString* identity=file.URLByResolvingSymlinksInPath.URLByStandardizingPath.path;
            if (games[identity] || [hidden containsObject:file.URLByStandardizingPath.path]) return;
            CaraLibraryGame* game=[CaraLibraryGame new]; game.url=file;
            game.title=file.lastPathComponent.stringByDeletingPathExtension;
            game.source=source; game.fileSize=[values[NSURLFileSizeKey] unsignedLongLongValue];
            game.modified=values[NSURLContentModificationDateKey]?:NSDate.distantPast;
            games[identity]=game;
        };
        void (^folder)(NSURL*,BOOL,NSString*)=^(NSURL* root,BOOL recursive,NSString* name) {
            NSError* error=nil;
            // Coordinated enumeration works with local and File Provider folders.
            [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateReadingItemAtURL:root options:0 error:&error byAccessor:^(NSURL* actual) {
                NSDirectoryEnumerator* enumerator=[NSFileManager.defaultManager enumeratorAtURL:actual includingPropertiesForKeys:keys options:NSDirectoryEnumerationSkipsHiddenFiles|NSDirectoryEnumerationSkipsPackageDescendants errorHandler:^BOOL(NSURL* item,NSError* problem) { failure=problem.localizedDescription; return NO; }];
                for (NSURL* item in enumerator) {
                    NSNumber* isDirectory=nil; NSNumber* isLink=nil;
                    [item getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];
                    [item getResourceValue:&isLink forKey:NSURLIsSymbolicLinkKey error:nil];
                    if (isLink.boolValue || (isDirectory.boolValue && !recursive)) [enumerator skipDescendants];
                    if (!isDirectory.boolValue && !isLink.boolValue) add(item,name);
                }
            }];
            if (error) failure=[NSString stringWithFormat:@"%@ is unavailable. Choose the folder again in Settings.",name];
        };
        folder(self->_defaultFolder,YES,@"Roms");
        // Preserve games from previous Cara releases without relocating any files.
        folder(self->documents,NO,@"Cara");
        if (custom && ![custom.URLByStandardizingPath isEqual:self->_defaultFolder.URLByStandardizingPath]) folder(custom,YES,custom.lastPathComponent);
        for (NSData* data in imported) {
            if (![data isKindOfClass:NSData.class]) continue;
            NSURL* file=[NSURL URLByResolvingBookmarkData:data options:NSURLBookmarkResolutionWithoutUI relativeToURL:nil bookmarkDataIsStale:nil error:nil];
            BOOL access=[file startAccessingSecurityScopedResource];
            if (file) add(file,@"Files");
            if (access) [file stopAccessingSecurityScopedResource];
        }
        NSArray* result=[games.allValues sortedArrayUsingComparator:^NSComparisonResult(CaraLibraryGame* a,CaraLibraryGame* b) { return [a.title localizedStandardCompare:b.title]; }];
        if (scoped) [custom stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(),^{ if (generation==self->scanGeneration) completion(result,failure); });
    } });
}
- (void)dealloc { if (folderScoped) [selectedFolder stopAccessingSecurityScopedResource]; }
@end
