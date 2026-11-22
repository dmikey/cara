// SPDX-License-Identifier: GPL-2.0-or-later
#import "core/frontend/apple/nearby_streetpass.h"
#import <MultipeerConnectivity/MultipeerConnectivity.h>
#include "core/hle/service/cecd/streetpass.h"

using namespace Service::CECD;
static NSString* const ServiceType = @"cara-pass-v1";
static NSString* const Resource = @"encounter";

@interface CaraNearbyStreetPass () <MCSessionDelegate, MCNearbyServiceAdvertiserDelegate, MCNearbyServiceBrowserDelegate>
@end
@implementation CaraNearbyStreetPass {
    MCSession* session;
    MCNearbyServiceAdvertiser* advertiser;
    MCNearbyServiceBrowser* browser;
    NSMutableSet<MCPeerID*>* invited;
    NSMutableSet<MCPeerID*>* answered;
    NSMutableSet<MCPeerID*>* received;
    NSURL* nandURL;
    NSData* hello;
    NSString* detail;
}
- (NSString*)statusText { return detail ?: @"Off"; }
- (void)startWithNand:(NSURL*)nand {
    if (session) return;
    NSMutableData* titles = [NSMutableData dataWithBytes:"CP01" length:4];
    try {
        for (auto title : RegisteredStreetPassTitles(nand.fileSystemRepresentation)) {
            const auto word = CFSwapInt32HostToLittle(title);
            [titles appendBytes:&word length:sizeof(word)];
        }
    } catch (const std::exception& e) { detail = [NSString stringWithUTF8String:e.what()]; return; }
    if (titles.length == 4) { detail = @"Enable StreetPass inside a game first."; return; }
    nandURL = nand;
    hello = titles;
    invited = [NSMutableSet new]; answered = [NSMutableSet new]; received = [NSMutableSet new];
    MCPeerID* peer = [[MCPeerID alloc] initWithDisplayName:NSUUID.UUID.UUIDString];
    session = [[MCSession alloc] initWithPeer:peer securityIdentity:nil encryptionPreference:MCEncryptionRequired];
    session.delegate = self;
    advertiser = [[MCNearbyServiceAdvertiser alloc] initWithPeer:peer discoveryInfo:nil serviceType:ServiceType];
    browser = [[MCNearbyServiceBrowser alloc] initWithPeer:peer serviceType:ServiceType];
    advertiser.delegate = self; browser.delegate = self;
    [advertiser startAdvertisingPeer]; [browser startBrowsingForPeers];
    detail = @"Looking for nearby Cara libraries.";
}
- (void)stop {
    [browser stopBrowsingForPeers]; [advertiser stopAdvertisingPeer]; [session disconnect];
    browser.delegate = nil; advertiser.delegate = nil; session.delegate = nil;
    browser = nil; advertiser = nil; session = nil; hello = nil; detail = nil;
}
- (void)browser:(MCNearbyServiceBrowser*)b foundPeer:(MCPeerID*)peer withDiscoveryInfo:(NSDictionary*)info {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (b != self->browser || self->invited.count >= 32 || [self->invited containsObject:peer]) return;
        // Exactly one end initiates each connection, even during simultaneous discovery.
        if ([self->session.myPeerID.displayName compare:peer.displayName] != NSOrderedAscending) return;
        [self->invited addObject:peer];
        [b invitePeer:peer toSession:self->session withContext:nil timeout:15];
    });
}
- (void)browser:(MCNearbyServiceBrowser*)b lostPeer:(MCPeerID*)peer {}
- (void)advertiser:(MCNearbyServiceAdvertiser*)a didReceiveInvitationFromPeer:(MCPeerID*)peer
       withContext:(NSData*)context invitationHandler:(void(^)(BOOL, MCSession*))accept {
    dispatch_async(dispatch_get_main_queue(), ^{
        BOOL ok = a == self->advertiser && self->invited.count < 32 && ![self->invited containsObject:peer] &&
                  [self->session.myPeerID.displayName compare:peer.displayName] == NSOrderedDescending;
        if (ok) [self->invited addObject:peer];
        accept(ok, ok ? self->session : nil);
    });
}
- (void)browser:(MCNearbyServiceBrowser*)b didNotStartBrowsingForPeers:(NSError*)error {
    dispatch_async(dispatch_get_main_queue(), ^{ if (b == self->browser) self->detail = error.localizedDescription; });
}
- (void)advertiser:(MCNearbyServiceAdvertiser*)a didNotStartAdvertisingPeer:(NSError*)error {
    dispatch_async(dispatch_get_main_queue(), ^{ if (a == self->advertiser) self->detail = error.localizedDescription; });
}
- (void)session:(MCSession*)s peer:(MCPeerID*)peer didChangeState:(MCSessionState)state {
    if (state != MCSessionStateConnected) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (s != self->session) return;
        NSError* error = nil;
        if (![s sendData:self->hello toPeers:@[peer] withMode:MCSessionSendDataReliable error:&error])
            self->detail = error.localizedDescription;
    });
}
- (void)session:(MCSession*)s didReceiveData:(NSData*)data fromPeer:(MCPeerID*)peer {
    if (data.length < 8 || data.length > 4 + 24 * 4 || data.length % 4 || memcmp(data.bytes,"CP01",4)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (s != self->session || ![self->invited containsObject:peer] || [self->answered containsObject:peer]) return;
        [self->answered addObject:peer];
        StreetPassTitles titles;
        for (NSUInteger i=4; i<data.length; i+=4) {
            uint32_t word;
            [data getBytes:&word range:NSMakeRange(i,4)];
            titles.insert(CFSwapInt32LittleToHost(word));
        }
        NSURL* file = [[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString];
        try {
            if (!ExportStreetPass(self->nandURL.fileSystemRepresentation,file.fileSystemRepresentation,&titles)) return;
            [s sendResourceAtURL:file withName:Resource toPeer:peer withCompletionHandler:^(NSError* error) {
                [NSFileManager.defaultManager removeItemAtURL:file error:nil];
            }];
        } catch (const std::exception& e) { self->detail = [NSString stringWithUTF8String:e.what()]; }
    });
}
- (void)session:(MCSession*)s didStartReceivingResourceWithName:(NSString*)name fromPeer:(MCPeerID*)peer
   withProgress:(NSProgress*)progress {
    if (![name isEqual:Resource] || progress.totalUnitCount > int64_t(MaxStreetPassFile)) [progress cancel];
}
- (void)session:(MCSession*)s didFinishReceivingResourceWithName:(NSString*)name fromPeer:(MCPeerID*)peer
          atURL:(NSURL*)url withError:(NSError*)error {
    if (error || !url || ![name isEqual:Resource]) return;
    // Multipeer deletes the temporary URL when this delegate returns. Move it
    // locally before dispatching; the main thread checks session generation
    // again before writing any guest data.
    NSNumber* size = nil;
    if (![url getResourceValue:&size forKey:NSURLFileSizeKey error:nil] ||
        !size || size.unsignedLongLongValue > MaxStreetPassFile) return;
    NSURL* copy = [[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString];
    if (![NSFileManager.defaultManager moveItemAtURL:url toURL:copy error:nil]) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (s == self->session && [self->invited containsObject:peer] && ![self->received containsObject:peer]) {
            [self->received addObject:peer];
            try {
                const auto count = ImportStreetPass(self->nandURL.fileSystemRepresentation,copy.fileSystemRepresentation,std::time(nullptr));
                self->detail = [NSString stringWithFormat:@"Last encounter delivered %u new messages.", count];
            } catch (const std::exception& e) { self->detail = [NSString stringWithUTF8String:e.what()]; }
        }
        [NSFileManager.defaultManager removeItemAtURL:copy error:nil];
    });
}
- (void)session:(MCSession*)s didReceiveStream:(NSInputStream*)stream withName:(NSString*)name
       fromPeer:(MCPeerID*)peer { [stream close]; }
@end

#if TARGET_OS_OSX
#include <iostream>
namespace Cara {
int RunNearbyStreetPass(const std::filesystem::path& nand, unsigned seconds) {
    @autoreleasepool {
        if (RegisteredStreetPassTitles(nand).empty()) {
            std::cerr << "Enable StreetPass inside a game first.\n";
            return 1;
        }
        auto pass = [CaraNearbyStreetPass new];
        [pass startWithNand:[NSURL fileURLWithFileSystemRepresentation:nand.c_str()
                    isDirectory:YES relativeToURL:nil]];
        NSDate* end = [NSDate dateWithTimeIntervalSinceNow:seconds];
        while (end.timeIntervalSinceNow > 0)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        std::cout << pass.statusText.UTF8String << '\n';
        [pass stop];
        return 0;
    }
}
} // namespace Cara
#endif
