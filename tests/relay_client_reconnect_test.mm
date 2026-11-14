// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
// Two independent Foundation processes, explicit live endpoint; credentials stdin only.
#import <Foundation/Foundation.h>
#include <chrono>
#include <functional>
#include <iostream>
#include <set>
#include <thread>
#include "network/relay_client.h"
using Client=Network::RelayClient;
static bool Await(const std::function<bool()>& test,unsigned seconds){
 auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
 do{if(test())return true;std::this_thread::sleep_for(std::chrono::milliseconds(5));}while(std::chrono::steady_clock::now()<end);return false;
}
int main(){@autoreleasepool{
 std::string line;std::getline(std::cin,line);if(line.size()>8192)return 2;
 NSData* bytes=[NSData dataWithBytes:line.data() length:line.size()];NSDictionary* values=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];line.clear();
 if(![values isKindOfClass:NSDictionary.class])return 2;
 NSString* room=values[@"room"];NSString* password=values[@"password"];NSString* role=values[@"role"];
 if(![room isKindOfClass:NSString.class]||![password isKindOfClass:NSString.class]||![role isKindOfClass:NSString.class])return 2;
 Client::Config config;config.room=room.UTF8String;config.password=password.UTF8String;
 auto client=Client::Create();const auto local=client->GetLocalMac();
 if(!client->Connect(config)||!Await([&]{return client->GetStatus().state==Client::State::Connected;},20))return 3;
 std::cout<<"READY\n"<<std::flush;
 if([role isEqualToString:@"rejoin"]){
  std::getline(std::cin,line);if(line!="go")return 4;
  for(unsigned i=0;i<40;++i){
   if(i && (!client->Connect(config)||!Await([&]{return client->GetStatus().state==Client::State::Connected;},20)))return 5;
   if(client->GetLocalMac()!=local)return 6;
   Client::Packet p;p.kind=Cara::Relay::Kind::Authentication;p.channel=11;p.destination=Cara::Relay::Broadcast;p.payload={0,0,1,0,0,0};
   if(!client->TrySend(p))return 7;
   if(!Await([&]{for(const auto& item:client->Drain())if(!item.peer_left&&item.packet.payload==Cara::Relay::Bytes({0,0,2,0,0,0}))return true;return false;},10))return 8;
  }
  std::cout<<"PASS 40 reconnect epochs preserved local MAC and exchanged replies\n";
 }else if([role isEqualToString:@"observer"]){
  std::uint64_t previous=0;Client::Mac peer_mac{};bool retired=false,invalid=false;unsigned count=0;std::set<std::uint64_t> ids;
  const bool complete=Await([&]{
   for(const auto& item:client->Drain()){
    if(item.peer_left){if(item.source_id==previous)retired=true;continue;}
    if(item.packet.payload!=Cara::Relay::Bytes({0,0,1,0,0,0})||(!retired&&previous)||ids.count(item.source_id)||(previous&&peer_mac!=item.packet.transmitter)){invalid=true;return true;}
    ids.insert(item.source_id);previous=item.source_id;peer_mac=item.packet.transmitter;retired=false;++count;
    Client::Packet ack;ack.kind=Cara::Relay::Kind::Authentication;ack.channel=11;ack.destination=Cara::Relay::Broadcast;ack.payload={0,0,2,0,0,0};if(!client->TrySend(ack)){invalid=true;return true;}
   }
   return count==40;
  },100);
  if(!complete||invalid||count!=40)return 9;
  if(!Await([&]{return client->GetStatus().sent==40;},5))return 10;
  std::cout<<"PASS 40 unique source IDs; trusted leave precedes each same-MAC rebind\n";
 }else return 11;
 client->Disconnect();return 0;
}}
