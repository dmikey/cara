// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/home_nand.h"

#include "core/file_sys/title_metadata.h"
#include "core/hle/service/fs/archive.h"
#include "core/loader/loader.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

static NSString* SystemID() { return @(SYSTEM_ID); }

static NSURL* NANDDirectory() {
    NSURL* documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    return [documents URLByAppendingPathComponent:@"cara/nand" isDirectory:YES];
}

static NSURL* TitleDirectory(NSURL* nand) {
    return [nand URLByAppendingPathComponent:[SystemID() stringByAppendingPathComponent:@"title"] isDirectory:YES];
}

static NSError* ImportError(NSString* message) {
    return [NSError errorWithDomain:@"CaraHomeNAND" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];
}

static BOOL IsHex(NSString* name, NSUInteger size) {
    NSCharacterSet* invalid=[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet];
    return name.length==size && [name rangeOfCharacterFromSet:invalid].location==NSNotFound;
}

static BOOL ValidateTree(NSURL* root, NSError** error) {
    NSArray* keys=@[NSURLIsSymbolicLinkKey,NSURLIsDirectoryKey,NSURLIsRegularFileKey];
    __block NSError* failure=nil;
    NSDirectoryEnumerator* entries=[NSFileManager.defaultManager enumeratorAtURL:root includingPropertiesForKeys:keys options:0 errorHandler:^BOOL(NSURL* item,NSError* issue) {
        failure=issue;
        return NO;
    }];
    NSDictionary* rootValues=[root resourceValuesForKeys:keys error:&failure];
    if (![rootValues[NSURLIsDirectoryKey] boolValue] || [rootValues[NSURLIsSymbolicLinkKey] boolValue] || !entries) {
        if (error) *error=failure?:ImportError(@"Select the folder containing your extracted, decrypted NAND.");
        return NO;
    }
    for (NSURL* item in entries) {
        NSDictionary* values=[item resourceValuesForKeys:keys error:&failure];
        if (failure) break;
        if ([values[NSURLIsSymbolicLinkKey] boolValue] ||
            (![values[NSURLIsDirectoryKey] boolValue] && ![values[NSURLIsRegularFileKey] boolValue])) {
            failure=ImportError(@"The NAND folder contains a link or unsupported file.");
            break;
        }
        if ([item.pathExtension isEqualToString:@"tmd"] && !IsHex(item.lastPathComponent.stringByDeletingPathExtension,8)) {
            failure=ImportError(@"The NAND folder contains an invalid title metadata filename.");
            break;
        }
    }
    if (error) *error=failure;
    return failure==nil;
}

static NSURL* MenuContent(NSURL* titleRoot, unsigned* region, u64* program=nullptr) {
    // Same regional HOME Menu titles used by the core's APT service.
    constexpr unsigned lowIDs[]={0x8202,0x8f02,0x9802,0,0xa102,0xa902,0xb102};
    NSFileManager* files=NSFileManager.defaultManager;
    for (unsigned index=0;index<std::size(lowIDs);++index) {
        if (!lowIDs[index]) continue;
        NSURL* content=[titleRoot URLByAppendingPathComponent:[NSString stringWithFormat:@"00040030/%08x/content",lowIDs[index]] isDirectory:YES];
        NSArray<NSString*>* names=[[files contentsOfDirectoryAtPath:content.path error:nil] sortedArrayUsingSelector:@selector(compare:)];
        NSString* metadata=nil;
        for (NSString* name in names) {
            if ([name.pathExtension isEqualToString:@"tmd"] && IsHex(name.stringByDeletingPathExtension,8)) { metadata=name; break; }
        }
        if (!metadata) continue;
        NSURL* tmdURL=[content URLByAppendingPathComponent:metadata];
        NSNumber* tmdSize=nil;
        if (![tmdURL getResourceValue:&tmdSize forKey:NSURLFileSizeKey error:nil] || tmdSize.unsignedLongLongValue>4*1024*1024) continue;
        FileSys::TitleMetadata tmd;
        const u64 titleID=0x0004003000000000ULL | lowIDs[index];
        if (tmd.Load(tmdURL.fileSystemRepresentation)!=Loader::ResultStatus::Success ||
            tmd.GetTitleID()!=titleID || !tmd.GetContentCount()) continue;
        // Match the existing AM title resolver's handling of optional content.
        if (tmd.GetContentCount()>1 && (tmd.GetContentTypeByIndex(1)&FileSys::TMDContentTypeFlag::Optional))
            content=[content URLByAppendingPathComponent:@"00000000" isDirectory:YES];
        NSURL* app=[content URLByAppendingPathComponent:[NSString stringWithFormat:@"%08x.app",tmd.GetContentIDByIndex(0)]];
        NSFileHandle* file=[NSFileHandle fileHandleForReadingFromURL:app error:nil];
        NSData* header=[file readDataUpToLength:512 error:nil];
        [file closeFile];
        if (header.length!=512) continue;
        const auto* bytes=static_cast<const uint8_t*>(header.bytes);
        if (memcmp(bytes+0x100,"NCCH",4) || !(bytes[0x18d]&2) || !(bytes[0x18f]&4)) continue;
        u64 programID=0;
        for (unsigned byte=0;byte<8;++byte) programID|=u64(bytes[0x118+byte])<<(8*byte);
        if (programID!=titleID) continue;
        *region=index;
        if (program) *program=titleID;
        return app;
    }
    return nil;
}

// Conflicting title files may be two different revisions. Never mix their bytes.
static BOOL MergeNANDTree(NSURL* source, NSURL* destination, BOOL preserveExisting, NSError** error) {
    NSFileManager* files=NSFileManager.defaultManager;
    BOOL destinationDirectory=NO;
    if (![files fileExistsAtPath:destination.path isDirectory:&destinationDirectory])
        return [files copyItemAtURL:source toURL:destination error:error];

    BOOL sourceDirectory=NO;
    if (![files fileExistsAtPath:source.path isDirectory:&sourceDirectory]) {
        *error=ImportError(@"A file disappeared from the NAND folder during import.");
        return NO;
    }
    if (sourceDirectory && destinationDirectory) {
        NSArray<NSURL*>* entries=[files contentsOfDirectoryAtURL:source includingPropertiesForKeys:nil options:0 error:error];
        if (!entries) return NO;
        for (NSURL* item in entries) {
            if (!MergeNANDTree(item,[destination URLByAppendingPathComponent:item.lastPathComponent],preserveExisting,error))
                return NO;
        }
        return YES;
    }
    if (!sourceDirectory && !destinationDirectory &&
        (preserveExisting || [files contentsEqualAtPath:source.path andPath:destination.path])) return YES;

    *error=ImportError([NSString stringWithFormat:@"This NAND conflicts with an existing Cara file (%@). Your current NAND was kept.",destination.lastPathComponent]);
    return NO;
}

static NSURL* NANDDataDirectory(NSURL* nand) {
    NSURL* data=[nand URLByAppendingPathComponent:@"data" isDirectory:YES];
    NSFileManager* files=NSFileManager.defaultManager;
    NSURL* canonical=[data URLByAppendingPathComponent:SystemID() isDirectory:YES];
    if ([files fileExistsAtPath:canonical.path]) return canonical;

    NSURL* match=nil;
    for (NSString* name in [files contentsOfDirectoryAtPath:data.path error:nil]) {
        if (!IsHex(name,32)) continue;
        if (match) return nil;
        match=[data URLByAppendingPathComponent:name isDirectory:YES];
    }
    return match;
}

@implementation CaraHomeNAND
+ (instancetype)menuInNAND:(NSURL*)nand {
    unsigned region=0;
    u64 titleID=0;
    NSURL* app=MenuContent(TitleDirectory(nand),&region,&titleID);
    if (!app) return nil;
    CaraHomeNAND* menu=[self new];
    menu->_menuURL=app;
    menu->_region=region;
    menu->_titleID=titleID;
    return menu;
}

+ (instancetype)installedMenu { return [self menuInNAND:NANDDirectory()]; }

+ (instancetype)importFolder:(NSURL*)folder error:(NSError**)error {
    BOOL scoped=[folder startAccessingSecurityScopedResource];
    __block CaraHomeNAND* menu=nil;
    __block NSError* failure=nil;
    NSError* coordinationError=nil;
    NSFileCoordinator* coordinator=[[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coordinator coordinateReadingItemAtURL:folder options:0 error:&coordinationError byAccessor:^(NSURL* actual) {
        if (!ValidateTree(actual,&failure)) return;
        NSURL* active=NANDDirectory();
        if ([actual.URLByResolvingSymlinksInPath isEqual:active.URLByResolvingSymlinksInPath]) {
            menu=[self installedMenu];
            if (!menu) failure=ImportError(@"Cara’s NAND does not contain a readable HOME Menu.");
            return;
        }
        NSFileManager* files=NSFileManager.defaultManager;
        NSURL* sourceNAND=nil;
        NSURL* titleRoot=nil;
        NSURL* incomingApp=nil;
        unsigned region=0;
        for (NSURL* candidate in @[[actual URLByAppendingPathComponent:@"nand" isDirectory:YES],actual]) {
            for (NSURL* titles in @[TitleDirectory(candidate),[candidate URLByAppendingPathComponent:@"title" isDirectory:YES]]) {
                incomingApp=MenuContent(titles,&region);
                if (incomingApp) { sourceNAND=candidate; titleRoot=titles; break; }
            }
            if (titleRoot) break;
        }
        if (!titleRoot) {
            failure=ImportError(@"No decrypted 3DS HOME Menu was found. Choose a NAND export folder containing title metadata and decrypted title contents.");
            return;
        }

        // Stage beside the active NAND so installing a validated import is one filesystem operation.
        NSURL* parent=active.URLByDeletingLastPathComponent;
        NSURL* staging=[parent URLByAppendingPathComponent:[@".nand-import-" stringByAppendingString:NSUUID.UUID.UUIDString] isDirectory:YES];
        const BOOL exists=[files fileExistsAtPath:active.path];
        if (exists && !ValidateTree(active,&failure)) return;
        if (![files createDirectoryAtURL:parent withIntermediateDirectories:YES attributes:nil error:&failure]) return;
        const BOOL prepared=exists?[files copyItemAtURL:active toURL:staging error:&failure]:
            [files createDirectoryAtURL:staging withIntermediateDirectories:NO attributes:nil error:&failure];
        NSURL* targetTitles=TitleDirectory(staging);
        if (prepared &&
            [files createDirectoryAtURL:targetTitles.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:&failure] &&
            MergeNANDTree(titleRoot,targetTitles,NO,&failure)) {
            NSURL* sourceData=NANDDataDirectory(sourceNAND);
            if (sourceData) {
                NSURL* targetData=[staging URLByAppendingPathComponent:@"data" isDirectory:YES];
                if ([files createDirectoryAtURL:targetData withIntermediateDirectories:YES attributes:nil error:&failure])
                    MergeNANDTree(sourceData,[targetData URLByAppendingPathComponent:SystemID() isDirectory:YES],YES,&failure);
            }
            if (!failure && ValidateTree(staging,&failure)) {
                CaraHomeNAND* staged=[self menuInNAND:staging];
                if (!staged || staged.region!=region ||
                    ![files contentsEqualAtPath:incomingApp.path andPath:staged.menuURL.path]) {
                    failure=ImportError(@"This NAND would conflict with Cara’s existing HOME Menu. Your current NAND was kept.");
                } else {
                    const unsigned flag=exists?RENAME_SWAP:RENAME_EXCL;
                    if (renamex_np(staging.fileSystemRepresentation,active.fileSystemRepresentation,flag)==0) {
                        menu=[self installedMenu];
                        if (exists && !menu &&
                            renamex_np(staging.fileSystemRepresentation,active.fileSystemRepresentation,RENAME_SWAP)!=0) {
                            failure=ImportError(@"The previous NAND remains in the import folder beside Cara’s NAND.");
                            return;
                        }
                    } else {
                        failure=[NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
                    }
                }
            }
        }
        [files removeItemAtURL:staging error:nil];
        if (!menu && !failure) failure=ImportError(@"The imported NAND did not contain a readable HOME Menu.");
    }];
    if (scoped) [folder stopAccessingSecurityScopedResource];
    if (error) *error=failure?:coordinationError;
    return menu;
}
@end
