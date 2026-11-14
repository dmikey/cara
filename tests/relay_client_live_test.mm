// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
// Explicit live transport test. Config/credentials ONLY on stdin, never printed.
#import <Foundation/Foundation.h>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include "network/relay_client.h"
using Client=Network::RelayClient;
static bool Await(const std::function<bool()>& test,unsigned seconds) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    do {if(test()) return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));} while(std::chrono::steady_clock::now()<end);
    return false;
}
static Cara::Relay::Bytes Payload() {
    Cara::Relay::Bytes p(12);
    for(unsigned i=0;i<15;++i) {p.insert(p.end(),{221,255});for(unsigned j=0;j<255;++j)p.push_back((i*13+j*37)&255);}
    p.insert(p.end(),{221,201});for(unsigned i=0;i<201;++i)p.push_back((i*19)&255);return p;
}
int main() {
    @autoreleasepool {
        std::string line;std::getline(std::cin,line);if(line.size()>8192) return 2;
        NSData* data=[NSData dataWithBytes:line.data() length:line.size()];
        NSDictionary* config=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];line.clear();
        if(![config isKindOfClass:NSDictionary.class]) return 2;
        NSString* room=config[@"room"];NSString* password=config[@"password"];NSString* role=config[@"role"];
        if(![room isKindOfClass:NSString.class] || ![password isKindOfClass:NSString.class] || ![role isKindOfClass:NSString.class]) return 2;
        auto client=Client::Create();Client::Config input;input.room=room.UTF8String;input.password=password.UTF8String;
        if(!client->Connect(std::move(input))) return 3;
        const bool bad=[role isEqualToString:@"bad"];
        if(!Await([&]{auto s=client->GetStatus();return bad?s.state==Client::State::Failed:s.state==Client::State::Connected;},20)) return 4;
        config=nil;password=nil;data=nil;
        if(bad) {if(client->GetStatus().failure!=Client::Failure::AuthenticationRejected) return 5;std::cout<<"PASS rejected admission\n";return 0;}
        std::cout<<"READY\n"<<std::flush;
        if([role isEqualToString:@"sender"]) {
            std::getline(std::cin,line);if(line!="go") return 6;
            Client::Packet packet;packet.kind=Cara::Relay::Kind::Beacon;packet.channel=11;packet.destination=Cara::Relay::Broadcast;packet.payload=Payload();
            if(!client->TrySend(packet)) return 7;
            bool unexpected=false;
            if(!Await([&]{auto q=client->Drain();bool reply=false;for(const auto& item:q){if(item.peer_left)continue;if(item.packet.kind==Cara::Relay::Kind::Authentication && item.packet.payload==Cara::Relay::Bytes({0,0,2,0,0,0}))reply=true;else unexpected=true;}return reply;},10) || unexpected) return 8;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));for(const auto& item:client->Drain())if(!item.peer_left)return 9;
            std::cout<<"PASS sent max envelope and received peer reply; no echo\n";
        } else if([role isEqualToString:@"receiver"]) {
            if(!Await([&]{auto q=client->Drain();return q.size()==1 && q[0].source_id && q[0].packet.transmitter!=client->GetLocalMac() && q[0].packet.kind==Cara::Relay::Kind::Beacon && q[0].packet.payload==Payload();},15)) return 10;
            Client::Packet ack;ack.kind=Cara::Relay::Kind::Authentication;ack.channel=11;ack.destination=Cara::Relay::Broadcast;ack.payload={0,0,2,0,0,0};
            if(!client->TrySend(ack) || !Await([&]{return client->GetStatus().sent==1;},5)) return 11;
            std::cout<<"PASS exact max payload/source and reply\n";
        } else if([role isEqualToString:@"isolated"]) {
            std::this_thread::sleep_for(std::chrono::seconds(6));if(!client->Drain().empty()) return 12;
            std::cout<<"PASS private room isolation\n";
        } else return 13;
        client->Disconnect();if(client->GetStatus().state!=Client::State::Disconnected) return 14;
        return 0;
    }
}
