// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/artwork_store.h"
#import <ImageIO/ImageIO.h>
#import <CommonCrypto/CommonDigest.h>
#include <cmath>

static const NSUInteger CaraArtworkMaxBytes=12*1024*1024;
static NSString* ArtworkKey(NSURL* game) {
    NSData* data=[game.URLByStandardizingPath.absoluteString dataUsingEncoding:NSUTF8StringEncoding];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(data.bytes,(CC_LONG)data.length,digest);
    NSMutableString* key=[NSMutableString new]; for(unsigned char byte : digest) [key appendFormat:@"%02x",byte]; return key;
}
static NSError* ArtworkError(NSString* message) { return [NSError errorWithDomain:@"CaraArtwork" code:1 userInfo:@{NSLocalizedDescriptionKey:message}]; }
static UIImage* ArtworkImage(NSData* data,NSUInteger maximum) {
    if(!data.length || data.length>CaraArtworkMaxBytes) return nil;
    CGImageSourceRef source=CGImageSourceCreateWithData((__bridge CFDataRef)data,nullptr); if(!source) return nil;
    NSDictionary* properties=CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source,0,nullptr));
    double width=[properties[(__bridge NSString*)kCGImagePropertyPixelWidth] doubleValue];
    double height=[properties[(__bridge NSString*)kCGImagePropertyPixelHeight] doubleValue];
    if(width<=0 || height<=0 || width*height>100000000) { CFRelease(source); return nil; }
    NSDictionary* options=@{(__bridge NSString*)kCGImageSourceCreateThumbnailFromImageAlways:@YES,(__bridge NSString*)kCGImageSourceCreateThumbnailWithTransform:@YES,(__bridge NSString*)kCGImageSourceThumbnailMaxPixelSize:@(maximum),(__bridge NSString*)kCGImageSourceShouldCacheImmediately:@YES};
    CGImageRef cg=CGImageSourceCreateThumbnailAtIndex(source,0,(__bridge CFDictionaryRef)options); CFRelease(source);
    UIImage* image=cg?[UIImage imageWithCGImage:cg]:nil; if(cg) CGImageRelease(cg); return image;
}

// Libretro often stores 3DS captures as a 400x480 two-screen stack.
// Extract the upper 400x240 screen only for that layout; custom art is untouched.
static UIImage* ArtworkHero(UIImage* image) {
    if(!image.CGImage) return image;
    size_t width=CGImageGetWidth(image.CGImage),height=CGImageGetHeight(image.CGImage);
    double ratio=double(width)/height;
    if(ratio<.82 || ratio>.85) return image;
    CGImageRef top=CGImageCreateWithImageInRect(image.CGImage,CGRectMake(0,0,width,floor(width*.6)));
    UIImage* result=top?[UIImage imageWithCGImage:top]:image; if(top) CGImageRelease(top); return result;
}

@implementation CaraArtworkStore {
    NSURL* root;
    NSMutableDictionary* indexes;
    NSMutableDictionary* indexAttempts;
    NSMutableSet* attemptedGames;
}
- (instancetype)initWithRoot:(NSURL*)url {
    if((self=[super init])) { root=url; indexes=[NSMutableDictionary new]; indexAttempts=[NSMutableDictionary new]; attemptedGames=[NSMutableSet new]; [NSFileManager.defaultManager createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:nil]; }
    NSURL* cache=[root URLByAppendingPathComponent:@"Cache"];
    [NSFileManager.defaultManager createDirectoryAtURL:cache withIntermediateDirectories:YES attributes:nil error:nil];
    [cache setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    return self;
}
+ (NSString*)normalizedTitle:(NSString*)title {
    NSString* stem=title.stringByDeletingPathExtension;
    NSRegularExpression* brackets=[NSRegularExpression regularExpressionWithPattern:@"\\([^)]*\\)|\\[[^]]*\\]" options:0 error:nil];
    stem=[brackets stringByReplacingMatchesInString:stem options:0 range:NSMakeRange(0,stem.length) withTemplate:@""];
    stem=[stem stringByFoldingWithOptions:NSDiacriticInsensitiveSearch|NSCaseInsensitiveSearch locale:[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]];
    NSArray* words=[stem componentsSeparatedByCharactersInSet:NSCharacterSet.alphanumericCharacterSet.invertedSet];
    NSMutableArray* clean=[NSMutableArray new]; for(NSString* word in words) if(word.length && ![word isEqual:@"the"]) [clean addObject:word];
    return [clean componentsJoinedByString:@""];
}
+ (NSString*)matchName:(NSArray<NSString*>*)names game:(NSURL*)game title:(NSString*)title {
    NSString* filename=game.lastPathComponent.stringByDeletingPathExtension;
    for(NSString* name in names) if([name.stringByDeletingPathExtension isEqualToString:filename]) return name;
    for(NSString* candidate in @[filename,title?:@""]) {
        NSString* key=[self normalizedTitle:candidate]; if(!key.length) continue;
        NSMutableArray* matches=[NSMutableArray new];
        for(NSString* name in names) if([[self normalizedTitle:name] isEqual:key]) [matches addObject:name];
        // A region match is safe; ambiguous cross-region matches retain the embedded icon.
        for(NSString* region in @[@"(USA)",@"(Europe)",@"(Japan)"]) if([filename containsString:region]) {
            NSArray* same=[matches filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSString* name,NSDictionary* bindings) { return [name containsString:region]; }]];
            if(same.count==1) return same.firstObject;
        }
        if(matches.count==1) return matches.firstObject;
    }
    return nil;
}
- (NSURL*)file:(NSString*)name game:(NSURL*)game custom:(BOOL)custom {
    NSURL* dir=[[root URLByAppendingPathComponent:custom?@"Custom":@"Cache"] URLByAppendingPathComponent:ArtworkKey(game)];
    [NSFileManager.defaultManager createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return [dir URLByAppendingPathComponent:name];
}
- (NSData*)download:(NSURL*)url {
    NSAssert(!NSThread.isMainThread,@"Artwork networking must stay off the main thread");
    if(![url.host isEqual:@"thumbnails.libretro.com"] || ![url.scheme isEqual:@"https"]) return nil;
    dispatch_semaphore_t done=dispatch_semaphore_create(0); __block NSData* result=nil;
    NSURLSessionConfiguration* config=NSURLSessionConfiguration.ephemeralSessionConfiguration; config.timeoutIntervalForRequest=8; config.timeoutIntervalForResource=12;
    NSURLSession* session=[NSURLSession sessionWithConfiguration:config];
    NSURLSessionDownloadTask* task=[session downloadTaskWithURL:url completionHandler:^(NSURL* file,NSURLResponse* response,NSError* error) {
        NSNumber* size=nil; [file getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        if(!error && [(NSHTTPURLResponse*)response statusCode]==200 && size.unsignedLongLongValue<=CaraArtworkMaxBytes)
            result=[NSData dataWithContentsOfURL:file options:NSDataReadingMappedIfSafe error:nil];
        dispatch_semaphore_signal(done);
    }];
    [task resume]; BOOL timedOut=dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,13*NSEC_PER_SEC))!=0;
    if(timedOut) [task cancel]; [session finishTasksAndInvalidate]; return timedOut?nil:result;
}
- (NSArray*)index:(NSString*)kind {
    if(indexes[kind]) return indexes[kind];
    NSURL* path=[root URLByAppendingPathComponent:[kind stringByAppendingString:@".plist"]];
    NSDictionary* saved=[NSDictionary dictionaryWithContentsOfURL:path]; NSArray* names=[saved[@"names"] isKindOfClass:NSArray.class]?saved[@"names"]:nil;
    NSDate* date=[saved[@"date"] isKindOfClass:NSDate.class]?saved[@"date"]:NSDate.distantPast;
    if(-date.timeIntervalSinceNow<7*24*3600) { indexes[kind]=names?:@[]; return indexes[kind]; }
    if(indexAttempts[kind]) return names?:@[]; indexAttempts[kind]=@YES;
    NSURL* url=[NSURL URLWithString:[NSString stringWithFormat:@"https://thumbnails.libretro.com/Nintendo%%20-%%20Nintendo%%203DS/%@/",kind]];
    NSString* html=[[NSString alloc] initWithData:[self download:url] encoding:NSUTF8StringEncoding];
    if(html.length) {
        NSRegularExpression* regex=[NSRegularExpression regularExpressionWithPattern:@"href=\"([^\"]+\\.png)\"" options:NSRegularExpressionCaseInsensitive error:nil];
        NSMutableArray* parsed=[NSMutableArray new];
        for(NSTextCheckingResult* match in [regex matchesInString:html options:0 range:NSMakeRange(0,html.length)]) {
            NSString* name=[[html substringWithRange:[match rangeAtIndex:1]] stringByRemovingPercentEncoding];
            if(name.length && [name.lastPathComponent isEqual:name] && ![name containsString:@".."] && ![name containsString:@":"]) [parsed addObject:name];
        }
        if(parsed.count) { names=parsed; [@{@"date":NSDate.date,@"names":names} writeToURL:path atomically:YES]; }
    }
    indexes[kind]=names?:@[]; return indexes[kind];
}
// Downloaded images are disposable; user overrides are never evicted.
- (void)trimCache {
    NSURL* cache=[root URLByAppendingPathComponent:@"Cache"];
    NSMutableArray* files=[NSMutableArray new]; unsigned long long total=0;
    for(NSURL* file in [NSFileManager.defaultManager enumeratorAtURL:cache includingPropertiesForKeys:@[NSURLFileSizeKey,NSURLContentModificationDateKey,NSURLIsRegularFileKey] options:NSDirectoryEnumerationSkipsHiddenFiles errorHandler:nil]) {
        NSDictionary* values=[file resourceValuesForKeys:@[NSURLFileSizeKey,NSURLContentModificationDateKey,NSURLIsRegularFileKey] error:nil];
        if([values[NSURLIsRegularFileKey] boolValue]) { total+=[values[NSURLFileSizeKey] unsignedLongLongValue]; [files addObject:@{@"url":file,@"size":values[NSURLFileSizeKey]?:@0,@"date":values[NSURLContentModificationDateKey]?:NSDate.distantPast}]; }
    }
    [files sortUsingComparator:^NSComparisonResult(NSDictionary* a,NSDictionary* b) { return [a[@"date"] compare:b[@"date"]]; }];
    for(NSDictionary* file in files) { if(total<=256ull*1024*1024) break; if([NSFileManager.defaultManager removeItemAtURL:file[@"url"] error:nil]) total-=[file[@"size"] unsignedLongLongValue]; }
}
- (NSDictionary*)artworkForURL:(NSURL*)game title:(NSString*)title {
    NSMutableDictionary* result=[NSMutableDictionary new]; NSString* key=ArtworkKey(game); BOOL downloadedAny=NO;
    for(NSString* kind in @[@"cover",@"hero"]) {
        NSString* file=[kind stringByAppendingString:@".jpg"];
        UIImage* image=ArtworkImage([NSData dataWithContentsOfURL:[self file:file game:game custom:YES]], [kind isEqual:@"hero"]?1920:1024);
        if(!image) image=ArtworkImage([NSData dataWithContentsOfURL:[self file:file game:game custom:NO]], [kind isEqual:@"hero"]?1920:1024);
        if(image) { result[kind]=image; continue; }
        NSString* attempt=[key stringByAppendingString:kind]; if([attemptedGames containsObject:attempt]) continue;
        [attemptedGames addObject:attempt];
        NSString* category=[kind isEqual:@"hero"]?@"Named_Snaps":@"Named_Boxarts";
        NSString* name=[CaraArtworkStore matchName:[self index:category] game:game title:title]; if(!name) continue;
        NSURL* base=[NSURL URLWithString:[NSString stringWithFormat:@"https://thumbnails.libretro.com/Nintendo%%20-%%20Nintendo%%203DS/%@/",category]];
        image=ArtworkImage([self download:[base URLByAppendingPathComponent:name]], [kind isEqual:@"hero"]?1920:1024);
        if(image && [kind isEqual:@"hero"]) image=ArtworkHero(image);
        if(image) { downloadedAny=YES; [UIImageJPEGRepresentation(image,.92) writeToURL:[self file:file game:game custom:NO] atomically:YES]; result[kind]=image; }
    }
    if(downloadedAny) [self trimCache];
    return result;
}
- (BOOL)importImage:(NSURL*)url forGame:(NSURL*)game hero:(BOOL)hero error:(NSError**)error {
    BOOL scoped=[url startAccessingSecurityScopedResource];
    NSNumber* size=nil; [url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    NSData* data=nil; if(size && size.unsignedLongLongValue<=CaraArtworkMaxBytes) data=[NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error];
    if(scoped) [url stopAccessingSecurityScopedResource];
    UIImage* image=ArtworkImage(data,hero?1920:1024);
    if(!image) { if(error) *error=ArtworkError(@"Choose a supported image smaller than 12 MB and 100 megapixels."); return NO; }
    return [UIImageJPEGRepresentation(image,.92) writeToURL:[self file:hero?@"hero.jpg":@"cover.jpg" game:game custom:YES] options:NSDataWritingAtomic error:error];
}
- (void)removeOverridesForGame:(NSURL*)game {
    for(NSString* file in @[@"cover.jpg",@"hero.jpg"]) [NSFileManager.defaultManager removeItemAtURL:[self file:file game:game custom:YES] error:nil];
}
@end
