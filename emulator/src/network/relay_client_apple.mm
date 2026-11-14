// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#import <Foundation/Foundation.h>
#import <Security/SecureTransport.h>
#include <cstdlib>
#include <mutex>
#include "network/relay_client.h"
#include "network/relay_session.h"

namespace Network::Detail {
struct AppleState {
    std::mutex mutex;
    RelaySession wire;
    __strong NSURLSession* session = nil;
    __strong NSURLSessionWebSocketTask* task = nil;
    __strong NSData* join = nil;
    bool sending{};
    std::string instance;
    AppleState() : wire([] { RelayClient::Mac mac{}; arc4random_buf(mac.data(),mac.size());
                            mac[0]=(mac[0]&0xfe)|2;return mac; }()) {
        unsigned char token[16];arc4random_buf(token,sizeof(token));
        for(auto byte:token) {instance.push_back("0123456789abcdef"[byte>>4]);instance.push_back("0123456789abcdef"[byte&15]);}
    }
};
static void Stop(const std::shared_ptr<AppleState>& s, std::uint64_t generation) {
    NSURLSession* session; NSURLSessionWebSocketTask* task;
    {
        std::lock_guard lock(s->mutex);
        if(s->wire.status.generation!=generation) return;
        session=s->session;task=s->task;s->session=nil;s->task=nil;s->join=nil;s->sending=false;
    }
    [task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeGoingAway reason:nil];
    [session invalidateAndCancel];
}
static void Fail(const std::shared_ptr<AppleState>& s, std::uint64_t generation, RelayClient::Failure failure) {
    {
        std::lock_guard lock(s->mutex);
        if(s->wire.status.generation!=generation ||
           (s->wire.status.state!=RelayClient::State::Connecting && s->wire.status.state!=RelayClient::State::Connected)) return;
        s->wire.Fail(failure);
        NotifyRelayWork();
    }
    Stop(s,generation);
}
static void Receive(const std::shared_ptr<AppleState>& s, std::uint64_t generation) {
    NSURLSessionWebSocketTask* task;
    {
        std::lock_guard lock(s->mutex);
        if(s->wire.status.generation!=generation || !s->task) return;
        task=s->task;
    }
    std::weak_ptr<AppleState> weak=s;
    [task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* message,NSError* error) {
        auto state=weak.lock();if(!state) return;
        if(error) {Fail(state,generation,RelayClient::Failure::TLSOrNetwork);return;}
        if(!message || message.type!=NSURLSessionWebSocketMessageTypeData || message.data.length>4101) {
            Fail(state,generation,RelayClient::Failure::ProtocolError);return;
        }
        bool okay;
        {
            std::lock_guard lock(state->mutex);
            if(state->wire.status.generation!=generation) return;
            const auto* bytes=static_cast<const std::uint8_t*>(message.data.bytes);
            Cara::Relay::Bytes chunk;
            if(message.data.length) chunk.assign(bytes,bytes+message.data.length);
            okay=state->wire.Feed(chunk);
            NotifyRelayWork();
        }
        if(okay) Receive(state,generation);else Stop(state,generation);
    }];
}
static void Pump(const std::shared_ptr<AppleState>& s) {
    NSURLSessionWebSocketTask* task;NSData* data;std::uint64_t generation;
    {
        std::lock_guard lock(s->mutex);
        if(s->sending || s->wire.status.state!=RelayClient::State::Connected || s->wire.outgoing.empty() || !s->task) return;
        s->sending=true;task=s->task;generation=s->wire.status.generation;
        const auto& frame=s->wire.outgoing.front();data=[NSData dataWithBytes:frame.data() length:frame.size()];
    }
    std::weak_ptr<AppleState> weak=s;
    [task sendMessage:[[NSURLSessionWebSocketMessage alloc] initWithData:data] completionHandler:^(NSError* error) {
        auto state=weak.lock();if(!state) return;
        if(error) {Fail(state,generation,RelayClient::Failure::TLSOrNetwork);return;}
        {
            std::lock_guard lock(state->mutex);
            if(state->wire.status.generation!=generation || state->wire.status.state!=RelayClient::State::Connected) return;
            state->wire.tx_bytes-=state->wire.outgoing.front().size();state->wire.outgoing.pop_front();
            ++state->wire.status.sent;state->wire.status.tx_queued=state->wire.outgoing.size();state->sending=false;
        }
        Pump(state);
    }];
}
} // namespace Network::Detail

@interface CaraRelayDelegate : NSObject<NSURLSessionWebSocketDelegate> {
@public
    std::weak_ptr<Network::Detail::AppleState> state;
    std::uint64_t generation;
}
@end
@implementation CaraRelayDelegate
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response newRequest:(NSURLRequest*)request
    completionHandler:(void (^)(NSURLRequest*))completionHandler {
    completionHandler(nil);
    if(auto s=state.lock()) Network::Detail::Fail(s,generation,Network::RelayClient::Failure::ProtocolError);
}
- (void)URLSession:(NSURLSession*)session webSocketTask:(NSURLSessionWebSocketTask*)task didOpenWithProtocol:(NSString*)protocol {
    auto s=state.lock();if(!s) return;
    NSData* join;
    {
        std::lock_guard lock(s->mutex);
        if(s->wire.status.generation!=generation || s->task!=task || !s->join) return;
        join=s->join;s->join=nil;
    }
    const auto current_generation=generation;std::weak_ptr<Network::Detail::AppleState> weak=s;
    [task sendMessage:[[NSURLSessionWebSocketMessage alloc] initWithData:join] completionHandler:^(NSError* error) {
        if(error) if(auto retained=weak.lock()) Network::Detail::Fail(retained,current_generation,Network::RelayClient::Failure::TLSOrNetwork);
    }];
    Network::Detail::Receive(s,generation);
}
- (void)URLSession:(NSURLSession*)session webSocketTask:(NSURLSessionWebSocketTask*)task
    didCloseWithCode:(NSURLSessionWebSocketCloseCode)closeCode reason:(NSData*)reason {
    if(auto s=state.lock()) Network::Detail::Fail(s,generation,Network::RelayClient::Failure::TLSOrNetwork);
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    if(error) if(auto s=state.lock()) Network::Detail::Fail(s,generation,Network::RelayClient::Failure::TLSOrNetwork);
}
@end

namespace Network {
class AppleRelayClient final : public RelayClient {
    std::shared_ptr<Detail::AppleState> state=std::make_shared<Detail::AppleState>();
    std::mutex lifecycle_mutex;
    void DisconnectLocked() {
        std::uint64_t generation;
        {std::lock_guard lock(state->mutex);state->wire.Disconnect();generation=state->wire.status.generation;}
        Detail::Stop(state,generation);
        Detail::NotifyRelayWork();
    }
public:
    ~AppleRelayClient() override {Disconnect();}
    bool Connect(Config config) override {
        @autoreleasepool {
            std::lock_guard lifecycle_lock(lifecycle_mutex);
            DisconnectLocked();
            auto string=[](const std::string& value) {return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];};
            NSString* endpoint=string(config.endpoint);NSString* room=string(config.room);NSString* password=string(config.password);
            NSURLComponents* parts=endpoint ? [NSURLComponents componentsWithString:endpoint] : nil;
            const bool valid=parts && [parts.scheme.lowercaseString isEqualToString:@"wss"] && parts.host.length &&
                [parts.path isEqualToString:@"/relay"] && !parts.user && !parts.password && !parts.query && !parts.fragment &&
                room && config.room.size()>=1 && config.room.size()<=64 && password && config.password.size()>=1 && config.password.size()<=512;
            if(!valid) {
                std::lock_guard lock(state->mutex);state->wire.Fail(Failure::InvalidConfiguration);return false;
            }
            NSData* json=[NSJSONSerialization dataWithJSONObject:@{@"version":@1,@"room":room,@"password":password,@"peer_events":@YES,@"instance":[NSString stringWithUTF8String:state->instance.c_str()]} options:0 error:nil];
            config.password.clear();password=nil;
            if(!json || json.length>4096 || !parts.URL) {
                std::lock_guard lock(state->mutex);state->wire.Fail(Failure::InvalidConfiguration);return false;
            }
            Cara::Relay::Bytes payload(static_cast<const std::uint8_t*>(json.bytes),static_cast<const std::uint8_t*>(json.bytes)+json.length);
            auto frame=Detail::RelaySession::Frame(1,payload);
            NSURLSessionConfiguration* settings=[NSURLSessionConfiguration ephemeralSessionConfiguration];
            settings.TLSMinimumSupportedProtocolVersion=tls_protocol_version_TLSv13;
            settings.HTTPShouldSetCookies=NO;settings.HTTPCookieStorage=nil;settings.URLCache=nil;settings.URLCredentialStorage=nil;
            settings.timeoutIntervalForRequest=15;settings.timeoutIntervalForResource=86400;
            NSOperationQueue* queue=[NSOperationQueue new];queue.maxConcurrentOperationCount=1;
            CaraRelayDelegate* delegate=[CaraRelayDelegate new];
            std::uint64_t generation;
            {
                std::lock_guard lock(state->mutex);state->wire.Begin();Detail::NotifyRelayWork();generation=state->wire.status.generation;
                delegate->state=state;delegate->generation=generation;
                state->join=[NSData dataWithBytes:frame.data() length:frame.size()];
                state->session=[NSURLSession sessionWithConfiguration:settings delegate:delegate delegateQueue:queue];
                NSMutableURLRequest* request=[NSMutableURLRequest requestWithURL:parts.URL];
                [request setValue:@"CaraRelayClient/1.0" forHTTPHeaderField:@"User-Agent"];
                state->task=[state->session webSocketTaskWithRequest:request];state->task.maximumMessageSize=4101;
                [state->task resume];
            }
            std::weak_ptr<Detail::AppleState> weak=state;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC),dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
                if(auto s=weak.lock()) {
                    bool pending=false;
                    {std::lock_guard lock(s->mutex);if(s->wire.status.generation==generation && s->wire.status.state==State::Connecting){s->wire.Fail(Failure::TLSOrNetwork);Detail::NotifyRelayWork();pending=true;}}
                    if(pending) Detail::Stop(s,generation);
                }
            });
            return true;
        }
    }
    void Disconnect() override {
        @autoreleasepool {
            std::lock_guard lifecycle_lock(lifecycle_mutex);
            DisconnectLocked();
        }
    }
    Status GetStatus() const override {std::lock_guard lock(state->mutex);return state->wire.status;}
    Mac GetLocalMac() const override {std::lock_guard lock(state->mutex);return state->wire.local;}
    bool SendImpl(const Packet& packet, std::optional<std::uint64_t> expected_generation) {
        bool accepted;std::uint64_t generation;bool failed;
        {std::lock_guard lock(state->mutex);accepted=expected_generation ? state->wire.SendForGeneration(packet,*expected_generation) : state->wire.Send(packet);generation=state->wire.status.generation;failed=state->wire.status.state==State::Failed;}
        if(failed) {Detail::NotifyRelayWork();Detail::Stop(state,generation);}else if(accepted) Detail::Pump(state);
        return accepted;
    }
    bool TrySend(const Packet& packet) override {return SendImpl(packet,std::nullopt);}
    bool TrySendForGeneration(const Packet& packet,std::uint64_t expected_generation) override {return SendImpl(packet,expected_generation);}
    std::vector<ReceivedPacket> Drain(std::size_t limit) override {std::lock_guard lock(state->mutex);return state->wire.Drain(limit);}
};
std::shared_ptr<RelayClient> RelayClient::Create() {return std::make_shared<AppleRelayClient>();}
} // namespace Network
