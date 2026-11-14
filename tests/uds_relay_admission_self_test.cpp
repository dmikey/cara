// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#include <cstdlib>
#include <iostream>
#include "network/uds_relay_admission.h"
using namespace Network::UDSRelay;
using namespace Cara::Relay;
static unsigned checks{};
static void Check(bool value){++checks;if(!value){std::cerr<<"admission check "<<checks<<" failed\n";std::abort();}}
static Mac M(unsigned id){return {2,0,0,0,0,static_cast<unsigned char>(id)};}
static Packet P(Kind kind,Bytes p,Mac tx=M(1)){return {kind,11,tx,Broadcast,std::move(p)};}
static Bytes Secure(unsigned source){Bytes p(22);p[0]=p[1]=0xaa;p[2]=3;p[6]=0x87;p[7]=0x6d;p[9]=14;p[13]=10;p[15]=1;p[19]=2;p[21]=source;return p;}
int main(){
    Snapshot client;client.role=Role::Client;client.local=M(2);client.host=M(1);client.channel=11;client.local_node=2;
    client.peer_count=2;client.peers[0]={M(2),2,true,true};client.peers[1]={M(3),3,true,true};
    auto map=P(Kind::NodeMap,{0,0});Check(Allows(map,client));map.transmitter=M(3);Check(!Allows(map,client));
    auto direct=P(Kind::Data,Secure(3),M(3));Check(Allows(direct,client));direct.payload[21]=2;Check(!Allows(direct,client));
    auto routed=P(Kind::Data,Secure(3));Check(Allows(routed,client));routed.payload[21]=4;Check(!Allows(routed,client));routed.payload[21]=1;Check(Allows(routed,client));
    auto wrong_host=client;wrong_host.host=M(4);Check(!Allows(map,wrong_host));Check(!Allows(routed,wrong_host));
    auto assoc=P(Kind::AssociationResponse,{0,0,0,0,1,0xc0});assoc.destination=M(2);Check(!Allows(assoc,client));
    auto joining=client;joining.role=Role::Connecting;Check(Allows(assoc,joining));assoc.transmitter=M(3);Check(!Allows(assoc,joining));
    auto host=client;host.role=Role::Hosting;host.local=M(1);host.local_node=1;host.maximum=4;host.total=2;host.storage_ready=host.free_node=true;
    host.peer_count=2;host.peers[0]={M(2),2,true,true};host.peers[1]={M(3),0,false,false};
    Bytes start(56);start[0]=start[1]=0xaa;start[2]=3;start[6]=0x88;start[7]=0x8e;start[8]=2;start[9]=1;start[11]=1;start[13]=1;
    auto eap=P(Kind::Data,start,M(3));Check(Allows(eap,host));
    auto full=host;full.total=full.maximum;Check(!Allows(eap,full));full=host;full.free_node=false;Check(!Allows(eap,full));full=host;full.storage_ready=false;Check(!Allows(eap,full));
    eap.transmitter=M(4);Check(!Allows(eap,host));eap.transmitter=M(2);Check(!Allows(eap,host));
    auto auth=P(Kind::Authentication,{0,0,1,0,0,0},M(4));Check(Allows(auth,host));auth.transmitter=M(3);Check(!Allows(auth,host));
    full=host;full.maximum=3;Check(!Allows(P(Kind::Authentication,{0,0,1,0,0,0},M(4)),full));
    auto deauth=P(Kind::Deauthentication,{},M(2));Check(Allows(deauth,host));host.peers[0].has_node_data=false;Check(!Allows(deauth,host));
    auto offline=client;offline.role=Role::Offline;auto beacon=P(Kind::Beacon,Bytes(12),M(4));beacon.channel=6;Check(Allows(beacon,offline));Check(!Allows(beacon,client));Check(!Allows(eap,offline));
    auto wrong_destination=P(Kind::Data,Secure(3),M(3));wrong_destination.destination=M(4);Check(!Allows(wrong_destination,client));
    for(unsigned source=0;source<256;++source){auto candidate=P(Kind::Data,Secure(source),M(3));Check(Allows(candidate,client)==(source==3));}
    Bytes logoff(672);logoff[0]=logoff[1]=0xaa;logoff[2]=3;logoff[6]=0x88;logoff[7]=0x8e;logoff[8]=2;logoff[9]=2;logoff[13]=2;logoff[26]=3;logoff[27]=4;logoff[65]=1;logoff[105]=3;logoff[145]=2;
    auto active=ActiveNodes(logoff);Check(bool(active));Check(active->mask==7&&active->slots[0]==1&&active->slots[1]==2&&active->slots[2]==3);
    auto malformed=logoff;malformed[105]=0;Check(!ActiveNodes(malformed));malformed=logoff;malformed[105]=2;Check(!ActiveNodes(malformed));
    malformed=logoff;malformed[145]=17;Check(!ActiveNodes(malformed));
    auto pending_host=host;pending_host.maximum=2;pending_host.total=1;pending_host.peer_count=1;pending_host.peers[0]={M(3),0,false,false};Check(!Allows(P(Kind::Authentication,{0,0,1,0,0,0},M(4)),pending_host));pending_host.peer_count=0;Check(Allows(P(Kind::Authentication,{0,0,1,0,0,0},M(4)),pending_host));
    std::cout<<"PASS "<<checks<<" UDS state/host/node/capacity/routing admission assertions\n";
}
