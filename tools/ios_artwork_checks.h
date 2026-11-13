// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/artwork_store.h"
static void CheckArtwork() {
    const auto check=[](BOOL ok,NSString* message) { if(!ok) @throw [NSException exceptionWithName:@"CaraArtworkTest" reason:message userInfo:nil]; };
    NSURL* game=[NSURL fileURLWithPath:@"/fixture/The Legend of Testing (USA).3ds"];
    check([[CaraArtworkStore normalizedTitle:@"Legend of Testing, The (USA) (En,Fr).png"] isEqual:[CaraArtworkStore normalizedTitle:game.lastPathComponent]],@"article, punctuation, region normalization");
    NSArray* names=@[@"Legend of Testing, The (USA).png",@"Legend of Testing, The (Europe).png"];
    check([[CaraArtworkStore matchName:names game:game title:@"Unrelated"] isEqual:names[0]],@"correct region");
    check([CaraArtworkStore matchName:names game:[NSURL fileURLWithPath:@"/fixture/Legend of Testing.3ds"] title:@""]==nil,@"ambiguous match rejected");
    check([CaraArtworkStore matchName:names game:[NSURL fileURLWithPath:@"/fixture/Another Game.3ds"] title:@""]==nil,@"wrong title rejected");
    NSURL* root=[[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString];
    [NSFileManager.defaultManager createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:nil];
    for(NSString* kind in @[@"Named_Boxarts",@"Named_Snaps"]) [@{@"date":NSDate.date,@"names":@[]} writeToURL:[root URLByAppendingPathComponent:[kind stringByAppendingString:@".plist"]] atomically:YES];
    CaraArtworkStore* store=[[CaraArtworkStore alloc] initWithRoot:root];
    UIGraphicsImageRendererFormat* format=[UIGraphicsImageRendererFormat defaultFormat]; format.scale=1;
    UIImage* original=[[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(3200,1800) format:format] imageWithActions:^(UIGraphicsImageRendererContext* c) { [UIColor.redColor setFill]; UIRectFill(CGRectMake(0,0,3200,1800)); }];
    NSURL* input=[root URLByAppendingPathComponent:@"input.jpg"]; [UIImageJPEGRepresentation(original,.9) writeToURL:input atomically:YES];
    NSError* error=nil; check([store importImage:input forGame:game hero:YES error:&error],@"custom image imported");
    NSDictionary* first=[store artworkForURL:game title:@"Testing"]; check([first[@"hero"] size].width==1920,@"hero downsampled"); check(first[@"cover"]==nil,@"independent cover fallback");
    CaraArtworkStore* reopened=[[CaraArtworkStore alloc] initWithRoot:root];
    check([reopened artworkForURL:game title:@"Testing"][@"hero"]!=nil,@"offline custom persistence across launches");
    NSURL* custom=[root URLByAppendingPathComponent:@"Custom"];
    NSString* key=[NSFileManager.defaultManager contentsOfDirectoryAtPath:custom.path error:nil].firstObject;
    NSURL* cached=[[root URLByAppendingPathComponent:@"Cache"] URLByAppendingPathComponent:key]; [NSFileManager.defaultManager createDirectoryAtURL:cached withIntermediateDirectories:YES attributes:nil error:nil];
    [NSFileManager.defaultManager copyItemAtURL:[[custom URLByAppendingPathComponent:key] URLByAppendingPathComponent:@"hero.jpg"] toURL:[cached URLByAppendingPathComponent:@"hero.jpg"] error:nil];
    [reopened removeOverridesForGame:game]; check([reopened artworkForURL:game title:@"Testing"][@"hero"]!=nil,@"reset falls back to disk cache");
    [@"not an image" writeToURL:input atomically:YES encoding:NSUTF8StringEncoding error:nil];
    check(![store importImage:input forGame:game hero:NO error:&error] && error!=nil,@"invalid import rejected");
    check([reopened artworkForURL:game title:@"Testing"][@"hero"]!=nil,@"invalid import preserves artwork");
    if([NSProcessInfo.processInfo.environment[@"CARA_ARTWORK_LIVE_CHECK"] isEqual:@"1"]) {
        NSURL* live=[NSURL fileURLWithPath:@"/fixture/Legend of Zelda, The - A Link Between Worlds (USA) (En,Fr,Es).3ds"];
        NSURL* liveRoot=[root URLByAppendingPathComponent:@"Live"]; CaraArtworkStore* online=[[CaraArtworkStore alloc] initWithRoot:liveRoot];
        NSDictionary* downloaded=[online artworkForURL:live title:@"The Legend of Zelda: A Link Between Worlds"];
        check(downloaded[@"cover"]!=nil && downloaded[@"hero"]!=nil,@"live Libretro cover and screenshot downloaded");
        check([downloaded[@"cover"] size].width>48,@"cover resolution exceeds embedded icon");
        check([downloaded[@"hero"] size].width/[downloaded[@"hero"] size].height>1.6,@"stacked capture uses upper screen for hero");
        CaraArtworkStore* fresh=[[CaraArtworkStore alloc] initWithRoot:liveRoot];
        NSDictionary* disk=[fresh artworkForURL:live title:@"The Legend of Zelda: A Link Between Worlds"];
        check(disk[@"cover"]!=nil && disk[@"hero"]!=nil,@"downloaded images persist across store instances");
        NSLog(@"Libretro live artwork passed: cover %@; hero %@",NSStringFromCGSize([disk[@"cover"] size]),NSStringFromCGSize([disk[@"hero"] size]));
    }
    [NSFileManager.defaultManager removeItemAtURL:root error:nil];
    NSLog(@"Cara artwork checks passed: matching, ambiguity, import bounds, persistence, offline cache, reset, invalid input.");
}
