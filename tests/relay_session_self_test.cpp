// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#include <cstdlib>
#include <iostream>
#include "network/relay_session.h"
using Network::RelayClient;
using Network::Detail::RelaySession;
using Bytes=Cara::Relay::Bytes;
static unsigned checks{};
static void Check(bool okay){++checks;if(!okay){std::cerr<<"relay check "<<checks<<" failed\n";std::abort();}}
static Bytes ID(unsigned id) {return {0,0,0,0,0,0,0,static_cast<unsigned char>(id)};}
static RelayClient::Packet Packet(unsigned last=2) {return {Cara::Relay::Kind::Authentication,11,{2,1,1,1,1,static_cast<unsigned char>(last)},Cara::Relay::Broadcast,{0,0,1,0,0,0}};}
static Bytes Incoming(unsigned source=2,unsigned mac=2) {auto body=ID(source);auto p=Cara::Relay::Encode(Packet(mac));body.insert(body.end(),p->begin(),p->end());return RelaySession::Frame(3,body);}
static Bytes Hello() {
    auto bytes=RelaySession::Frame(2,ID(1));auto ack=RelaySession::Frame(8,Bytes{1});bytes.insert(bytes.end(),ack.begin(),ack.end());
    for(unsigned id=2;id<34;++id){auto joined=RelaySession::Frame(7,ID(id));bytes.insert(bytes.end(),joined.begin(),joined.end());}
    return bytes;
}
static void Join(RelaySession& session) {session.Begin();Check(session.Feed(Hello()));}
int main() {
    const RelayClient::Mac local{2,1,1,1,1,1};
    const auto welcome=Hello();const auto packet=Incoming();
    // Every boundary, including header/body separation from Go websocket.NetConn.
    for(std::size_t cut=1;cut<welcome.size();++cut) {
        RelaySession s(local);s.Begin();Check(s.Feed(Bytes(welcome.begin(),welcome.begin()+cut)));Check(s.Feed(Bytes(welcome.begin()+cut,welcome.end())));Check(s.status.state==RelayClient::State::Connected);
    }
    for(std::size_t cut=1;cut<packet.size();++cut) {
        RelaySession s(local);Join(s);Check(s.Feed(Bytes(packet.begin(),packet.begin()+cut)));Check(s.Feed(Bytes(packet.begin()+cut,packet.end())));auto q=s.Drain(1);Check(q.size()==1&&q[0].source_id==2&&q[0].packet.payload==Packet().payload);Check(s.rx_bytes==0);
    }
    {RelaySession s(local);s.Begin();auto combined=welcome;combined.insert(combined.end(),packet.begin(),packet.end());Check(s.Feed(combined));Check(s.Drain(1).size()==1);}
    for(auto frame:{RelaySession::Frame(2,Bytes(7)),RelaySession::Frame(2,Bytes(8)),packet,Bytes{2,255,255,255,255},RelaySession::Frame(9,{})}) {
        RelaySession s(local);s.Begin();Check(!s.Feed(frame));Check(s.status.state==RelayClient::State::Failed);Check(s.incoming.empty()&&s.outgoing.empty());
    }
    {RelaySession s(local);Join(s);Check(!s.Feed(welcome));}
    {RelaySession s(local);Join(s);Check(!s.Feed(Incoming(1)));}
    {RelaySession s(local);Join(s);Check(!s.Feed(Incoming(2,1)));}
    {RelaySession s(local);Join(s);Check(s.Feed(packet));Check(!s.Feed(Incoming(2,3)));}
    {RelaySession s(local);Join(s);Check(s.Feed(packet));Check(!s.Feed(Incoming(3,2)));}
    {RelaySession s(local);Join(s);for(unsigned i=0;i<64;++i)Check(s.Feed(packet));Check(!s.Feed(packet));Check(s.status.failure==RelayClient::Failure::QueueLimit);Check(s.rx_bytes==0&&s.partial.empty());}
    {RelaySession s(local);Join(s);for(unsigned i=0;i<64;++i)Check(s.Feed(packet));Check(s.Drain(1000).size()==32);Check(s.status.rx_queued==32);Check(s.Drain(0).empty());Check(s.Drain().size()==32);Check(s.rx_bytes==0);}
    {RelaySession s(local);Join(s);for(unsigned i=0;i<64;++i)Check(s.Send(Packet()));Check(!s.Send(Packet()));Check(s.status.failure==RelayClient::Failure::QueueLimit);Check(s.outgoing.empty()&&s.tx_bytes==0);}
    {RelaySession s(local);Join(s);Check(s.Send(Packet(99)));auto decoded=Cara::Relay::Decode(Bytes(s.outgoing.front().begin()+5,s.outgoing.front().end()));Check(decoded&&decoded->transmitter==local);}
    {RelaySession s(local);Join(s);for(unsigned i=0;i<32;++i)Check(s.Feed(Incoming(i+2,i+2)));Check(!s.Feed(Incoming(40,40)));}
    {RelaySession s(local);Join(s);Check(s.Feed(Bytes(packet.begin(),packet.begin()+4)));Check(s.Send(Packet()));const auto old=s.status.generation;s.Disconnect();Check(s.status.generation>old&&s.partial.empty()&&s.incoming.empty()&&s.outgoing.empty()&&s.sources.empty());Check(!s.Feed(packet));Join(s);Check(s.Feed(packet));}
    {RelaySession s(local);s.Begin();Check(!s.Feed(RelaySession::Frame(5,Bytes{1,2,3})));Check(s.status.failure==RelayClient::Failure::AuthenticationRejected);}
    {RelaySession s(local);Join(s);Check(s.Feed(packet));s.Drain();Check(s.Feed(RelaySession::Frame(6,ID(2))));Check(s.sources.empty());
        Check(s.Feed(RelaySession::Frame(7,ID(40))));Check(s.Feed(Incoming(40,2)));auto q=s.Drain();Check(q.size()==2&&q[0].peer_left&&q[0].packet.transmitter==Packet().transmitter&&!q[1].peer_left&&q[1].source_id==40);Check(s.rx_bytes==0);}
    {RelaySession s(local);s.Begin();Check(s.Feed(RelaySession::Frame(2,ID(1))));Check(s.status.state==RelayClient::State::Connecting);Check(!s.Send(Packet()));Check(!s.Feed(packet));}
    {RelaySession s(local);s.Begin();Check(s.Feed(RelaySession::Frame(2,ID(1))));Check(s.Feed(RelaySession::Frame(8,Bytes{1})));
        for(unsigned id=2;id<82;++id){Check(s.Feed(RelaySession::Frame(7,ID(id))));Check(s.Feed(Incoming(id,2)));Check(s.Drain().size()==1);Check(s.Feed(RelaySession::Frame(6,ID(id))));auto q=s.Drain();Check(q.size()==1&&q[0].peer_left);Check(s.sources.empty()&&s.admitted.empty());}}
    {RelaySession s(local);s.status.state=RelayClient::State::Connected;s.status.generation=0;
        Check(!s.SendForGeneration(Packet(),1));Check(s.outgoing.empty());Check(s.SendForGeneration(Packet(),0));
        s.Clear();s.status.generation=1;Check(!s.SendForGeneration(Packet(),0));Check(s.outgoing.empty());Check(s.SendForGeneration(Packet(),1));}
    std::cout<<"PASS "<<checks<<" relay stream/bounds/identity/generation assertions\n";
}
