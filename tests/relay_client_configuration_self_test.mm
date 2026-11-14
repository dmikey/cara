// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#import <Foundation/Foundation.h>
#include <chrono>
#include <iostream>
#include <thread>
#include "network/relay_client.h"
int main(){@autoreleasepool{
 auto client=Network::RelayClient::Create();unsigned checks=0;
 for(const auto* endpoint:{"https://example.com/relay","ws://example.com/relay","wss:///relay","wss://@example.com/relay","wss://:password@example.com/relay","wss://example.com/relay?","wss://example.com/relay#","wss://example.com/other","wss://example.com/relay?room=a"}){
  Network::RelayClient::Config config;config.endpoint=endpoint;config.room="test";config.password="invalid-test-password";
  if(client->Connect(config)||client->GetStatus().failure!=Network::RelayClient::Failure::InvalidConfiguration)return 2;++checks;
 }
 for(auto size:{0u,65u}){Network::RelayClient::Config config;config.room.assign(size,'x');config.password="invalid-test-password";if(client->Connect(config))return 3;++checks;}
 Network::RelayClient::Config config;config.endpoint="WSS://127.0.0.1:1/relay";config.room="test";config.password="invalid-test-password";
 if(!client->Connect(config))return 4;++checks;client->Disconnect();const auto generation=client->GetStatus().generation;
 std::this_thread::sleep_for(std::chrono::milliseconds(250));if(client->GetStatus().state!=Network::RelayClient::State::Disconnected||client->GetStatus().generation!=generation)return 5;++checks;
 std::cout<<"PASS "<<checks<<" URL component/scheme/length/cancel-generation checks\n";
}}
