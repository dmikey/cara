// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
// ROM-free service-HLE witness. No production UI entry point or firmware.
#import <Foundation/Foundation.h>
#include <chrono>
#include <filesystem>
#include <deque>
#include <functional>
#include <iostream>
#include <optional>
#include <thread>
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/kernel/shared_page.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/service/nwm/nwm_uds.h"
#include "core/hle/service/nwm/uds_data.h"
#include "core/hle/service/sm/sm.h"
#include "core/settings.h"
#include "network/relay_client.h"
namespace {
void Check(bool okay,const char* reason){if(!okay)throw std::runtime_error(reason);}
class FixtureRelay final:public Network::RelayClient {
public:
 bool flip_on_drain{}, flip_on_send{};
 Mac local{2,0,0,0,0,1};Status status;std::deque<ReceivedPacket> rx;std::vector<Packet> sent;
 FixtureRelay(){status.state=State::Connected;status.generation=1;}
 bool Connect(Config)override{return false;}
 void Disconnect()override{status.state=State::Disconnected;++status.generation;rx.clear();Network::Detail::NotifyRelayWork();}
 Status GetStatus()const override{auto result=status;result.rx_queued=rx.size();return result;}
 Mac GetLocalMac()const override{return local;}
 bool TrySend(const Packet& packet)override{auto copy=packet;copy.transmitter=local;if(status.state!=State::Connected||!Cara::Relay::Valid(copy))return false;sent.push_back(std::move(copy));if(flip_on_send){flip_on_send=false;++status.generation;local[5]=91;Network::Detail::NotifyRelayWork();}return true;}
 std::vector<ReceivedPacket> Drain(std::size_t limit=MaxDrainPackets)override{std::vector<ReceivedPacket> result;while(!rx.empty()&&result.size()<std::min(limit,MaxDrainPackets)){result.push_back(std::move(rx.front()));rx.pop_front();}if(flip_on_drain){flip_on_drain=false;++status.generation;local[5]=92;Network::Detail::NotifyRelayWork();}return result;}
 void Emit(Packet p,bool departed=false){rx.push_back({2,std::move(p),departed});Network::Detail::NotifyRelayWork();}
};
class Window final:public Frontend::EmuWindow {
public:Window(){UpdateFramebufferLayout(400,480);}void PollEvents()override{}void MakeCurrent()override{}void DoneCurrent()override{}void SwapBuffers()override{}
};
std::vector<u8> Payload(bool response){std::vector<u8> bytes(512);for(unsigned i=0;i<bytes.size();++i)bytes[i]=(i*37+(response?91:17))&255;return bytes;}
}
namespace Service::NWM {
struct RelayHLETestDriver {
 static auto Context(NWM_UDS& uds,unsigned id,unsigned normal,unsigned translated){
  auto threads=uds.system.Kernel().GetCurrentThreadManager().GetThreadList();Check(!threads.empty(),"No generated-ELF thread");
  auto ctx=std::make_shared<Kernel::HLERequestContext>(uds.system.Kernel(),nullptr,threads.front());ctx->CommandBuffer()[0]=IPC::MakeHeader(id,normal,translated);return ctx;
 }
 static void Initialize(NWM_UDS& uds){
  auto memory=uds.system.Kernel().CreateSharedMemory(uds.system.Kernel().GetCurrentProcess().get(),0x4000,Kernel::MemoryPermission::ReadWrite,Kernel::MemoryPermission::ReadWrite);
  Check(memory.Succeeded(),"UDS shared-memory creation failed");NodeInfo node{};node.username[0]='P';node.username[1]='r';node.username[2]='o';node.username[3]='o';node.username[4]='f';
  Check(uds.Initialize(0x4000,node,0x100,memory.Unwrap()).Succeeded(),"Actual NWM Initialize failed");
 }
 static void Bind(NWM_UDS& uds,unsigned channel=1,unsigned id=1){
  auto ctx=Context(uds,0x12,4,0);auto* cb=ctx->CommandBuffer();cb[1]=id;cb[2]=0x4000;cb[3]=channel;cb[4]=0xffff;
  uds.Bind(*ctx);Check(cb[1]==0,"Actual NWM Bind failed");
 }
 static NetworkInfo Parameters(){NetworkInfo info{};info.channel=6;info.max_nodes=4;info.initialized=1;info.wlan_comm_id=0xCA123456;info.network_id=0xCA12;return info;}
 static void Host(NWM_UDS& uds){auto info=Parameters();Check(uds.BeginHostingNetwork(reinterpret_cast<const u8*>(&info),sizeof(info),{}).IsSuccess(),"Actual NWM BeginHosting failed");Bind(uds);}
 static bool FindHost(NWM_UDS& uds,NetworkInfo& info){
  std::lock_guard lock(uds.beacon_mutex);if(uds.received_beacons.empty())return false;
  info=Parameters();info.host_mac_address=uds.received_beacons.back().transmitter_address;return true;
 }
 static auto Connect(NWM_UDS& uds,const NetworkInfo& info){
  // Enter the busy generated guest so this test-only service call has a Running
  // requester, exactly as a real IPC dispatch does (never a Ready/sleeping thread).
  Check(uds.system.RunLoop()==Core::System::ResultStatus::Success,"Generated guest startup failed");
  auto ctx=Context(uds,0x1e,2,4);
  Check(uds.system.Kernel().GetCurrentThreadManager().GetThreadList().front()->status==Kernel::ThreadStatus::Running,"Connect fixture requester not running");
  uds.ConnectToNetwork(*ctx,0x1e,reinterpret_cast<const u8*>(&info),sizeof(info),0,{});
  uds.system.PrepareReschedule();return ctx;
 }
 static unsigned Status(NWM_UDS& uds){return uds.connection_status.status;}
 static unsigned Count(NWM_UDS& uds){return uds.connection_status.total_nodes;}
 static unsigned Node(NWM_UDS& uds){return uds.connection_status.network_node_id;}
 static bool HasPacket(NWM_UDS& uds){auto channel=uds.channel_data.find(1);return channel!=uds.channel_data.end()&&!channel->second.received_packets.empty();}
 static void Send(NWM_UDS& uds,unsigned destination,const std::vector<u8>& bytes){
  auto ctx=Context(uds,0x17,6,2);auto* cb=ctx->CommandBuffer();cb[1]=0;cb[2]=destination;cb[3]=1;cb[4]=0;cb[5]=bytes.size();cb[6]=0;cb[7]=IPC::StaticBufferDesc(bytes.size(),0);cb[8]=0;ctx->AddStaticBuffer(0,bytes);
  uds.SendTo(*ctx);Check(cb[1]==0,"Actual NWM SendTo failed");
 }
 static void Pull(NWM_UDS& uds,const std::vector<u8>& expected,unsigned source){
  auto ctx=Context(uds,0x14,3,0);auto* cb=ctx->CommandBuffer();cb[1]=1;cb[2]=0x172;cb[3]=1478;
  uds.PullPacket(*ctx);Check(cb[1]==0&&cb[2]==expected.size()&&cb[3]==source,"Actual NWM PullPacket metadata mismatch");
  const auto& bytes=ctx->GetStaticBuffer(0);Check(bytes.size()>=expected.size()&&std::equal(expected.begin(),expected.end(),bytes.begin()),"Actual NWM PullPacket bytes mismatch");
 }
 static void SpoofSource(NWM_UDS& uds){
  Network::RelayClient::Packet packet;packet.kind=Cara::Relay::Kind::Data;packet.channel=uds.network_channel;packet.destination=uds.network_info.host_mac_address;packet.payload=GenerateDataPayload({1,2,3,4},1,1,1,0);
  Check(uds.relay_session->TrySend(packet),"Spoof witness transport send failed");
 }
 static void ClearStatusEvent(NWM_UDS& uds){uds.connection_status_event->Clear();}
 static bool StatusSignalled(NWM_UDS& uds){return !uds.connection_status_event->ShouldWait(nullptr);}
 static Network::RelayClient::Mac M(unsigned id){return {2,0,0,0,0,static_cast<u8>(id)};}
 static void Offline(NWM_UDS& uds,FixtureRelay& relay){
  const auto ticks=uds.system.CoreTiming().GetGlobalTicks();
  auto packet=[&](Cara::Relay::Kind kind,std::vector<u8> data,unsigned tx=2){return Network::RelayClient::Packet{kind,11,M(tx),Cara::Relay::Broadcast,std::move(data)};};
  auto emit=[&](Network::RelayClient::Packet p,bool left=false){relay.Emit(std::move(p),left);Network::DrainRelayPackets(uds.system);};
  auto reset_host=[&]{uds.system.CoreTiming().UnscheduleEvent(uds.beacon_broadcast_event,0);uds.node_map.clear();relay.local=M(1);Initialize(uds);Host(uds);uds.network_channel=11;uds.network_info.channel=11;relay.sent.clear();};
  // Pending authentication departure frees capacity without a guest-node deauth/assert.
  reset_host();uds.connection_status.max_nodes=2;uds.network_info.max_nodes=2;uds.node_map[M(2)]={false,0};uds.connection_status_event->Clear();
  emit(packet(Cara::Relay::Kind::Deauthentication,{},2),true);Check(uds.node_map.empty()&&Count(uds)==1&&!StatusSignalled(uds),"Pending-auth departure leaked capacity or signalled a node loss");
  emit(packet(Cara::Relay::Kind::Authentication,{0,0,1,0,0,0},3));Check(uds.node_map.count(M(3))==1&&!uds.node_map[M(3)].connected&&relay.sent.size()==2,"Pending-auth capacity was not reusable");
  // Last slot consumed before EAPStart: no index/assert and no assigned node.
  reset_host();uds.connection_status.max_nodes=2;uds.connection_status.total_nodes=2;uds.connection_status.node_bitmask=3;uds.network_info.max_nodes=2;uds.network_info.total_nodes=2;uds.node_info[1].network_node_id=2;uds.node_map[M(2)]={true,2};uds.node_map[M(3)]={false,0};
  emit(packet(Cara::Relay::Kind::Data,GenerateEAPoLStartFrame(1,uds.current_node),3));Check(Count(uds)==2&&!uds.node_map[M(3)].connected,"Full host admitted pending EAPStart");
  // Sparse host departure followed by slot reuse must preserve the surviving node.
  reset_host();uds.connection_status.total_nodes=3;uds.network_info.total_nodes=3;uds.connection_status.node_bitmask=7;uds.connection_status.nodes[1]=2;uds.connection_status.nodes[2]=3;uds.node_info[1].network_node_id=2;uds.node_info[2].network_node_id=3;uds.node_map[M(2)]={true,2};uds.node_map[M(3)]={true,3};
  emit(packet(Cara::Relay::Kind::Deauthentication,{},2),true);Check(Count(uds)==2&&uds.node_info[1].network_node_id==3&&uds.node_info[2].network_node_id==0,"Host removal lost compact survivor prefix");
  uds.node_map[M(4)]={false,0};emit(packet(Cara::Relay::Kind::Data,GenerateEAPoLStartFrame(1,uds.current_node),4));Check(uds.node_info[1].network_node_id==3&&uds.node_info[2].network_node_id==2,"Rejoin overwrote survivor metadata");
  std::vector<u8> logoff;for(const auto& p:relay.sent)if(p.kind==Cara::Relay::Kind::Data&&p.payload.size()==672)logoff=p.payload;Check(!logoff.empty(),"Actual host did not emit EAPoLLogoff");
  relay.local=M(3);uds.connection_status.status=static_cast<u32>(NetworkStatus::ConnectedAsClient);uds.connection_status.network_node_id=3;uds.network_info.host_mac_address=M(1);uds.node_map.clear();uds.node_map[M(3)]={true,3};uds.node_map[M(4)]={true,2};
  emit(packet(Cara::Relay::Kind::Data,logoff,1));Check(uds.connection_status.nodes[0]==1&&uds.connection_status.nodes[1]==2&&uds.connection_status.nodes[2]==3&&uds.connection_status.node_bitmask==7,"Client indexed [1,3,2] records by position");
  emit(packet(Cara::Relay::Kind::Deauthentication,{},4),true);Check(uds.connection_status.nodes[1]==0&&uds.connection_status.nodes[2]==3&&Count(uds)==2,"Peer departure cleared survivor slot");
  // A peer learned solely from NodeMap is pruned by the host's authoritative update.
  relay.local=M(2);uds.connection_status.network_node_id=2;uds.connection_status.node_bitmask=7;uds.connection_status.nodes[0]=1;uds.connection_status.nodes[1]=2;uds.connection_status.nodes[2]=3;uds.connection_status.total_nodes=3;uds.network_info.total_nodes=3;uds.node_map.clear();uds.node_map[M(2)]={true,2};uds.node_map[M(3)]={true,3};
  std::vector<u8> map{0,1};auto local=M(2);map.insert(map.end(),local.begin(),local.end());map.insert(map.end(),{0,2});
  emit(packet(Cara::Relay::Kind::NodeMap,map,4));Check(uds.node_map.count(M(3))==1&&Count(uds)==3&&uds.network_info.host_mac_address==M(1),"Room peer replaced chosen host/NodeMap");
  emit(packet(Cara::Relay::Kind::NodeMap,map,1));Check(uds.node_map.count(M(3))==0&&Count(uds)==2&&uds.connection_status.nodes[2]==0,"Authoritative NodeMap left stale unbound-peer assignment");
  emit(packet(Cara::Relay::Kind::Data,GenerateDataPayload({1,2,3},1,2,3,0),1));Check(!HasPacket(uds),"Host-forwarded departed node remained admissible");
  // Bound actual downstream channel and total byte queues, then purge departed assignment.
  reset_host();uds.connection_status.total_nodes=2;uds.network_info.total_nodes=2;uds.connection_status.node_bitmask=3;uds.node_info[1].network_node_id=2;uds.node_map[M(2)]={true,2};Bind(uds,2,2);Bind(uds,3,3);
  for(unsigned channel=1;channel<=3;++channel)for(unsigned i=0;i<65;++i)emit(packet(Cara::Relay::Kind::Data,GenerateDataPayload(std::vector<u8>(1478,0x5a),channel,1,2,0),2));
  std::size_t bytes=0,count=0;for(const auto& [channel,binding]:uds.channel_data){Check(binding.received_packets.size()<=64,"UDS channel queue exceeded bound");for(const auto& p:binding.received_packets){bytes+=p.size();++count;}}
  Check(bytes<=Network::RelayClient::MaxQueueBytes&&count==174,"UDS global queue byte cap incorrect");emit(packet(Cara::Relay::Kind::Deauthentication,{},2),true);for(const auto& [channel,binding]:uds.channel_data)Check(binding.received_packets.empty(),"Departed source backlog survived assignment reuse");
  // A batch copied before a room transition must be erased before guest RunLoop.
  for(bool flip_during_send:{false,true}) {
   reset_host();uds.connection_status.total_nodes=2;uds.network_info.total_nodes=2;uds.connection_status.node_bitmask=3;uds.node_info[1].network_node_id=2;uds.node_map[M(2)]={true,2};
   if(flip_during_send) {relay.Emit(packet(Cara::Relay::Kind::Authentication,{0,0,1,0,0,0},3));relay.flip_on_send=true;}
   else relay.flip_on_drain=true;
   relay.Emit(packet(Cara::Relay::Kind::Data,GenerateDataPayload({0x45,0x50,0x4f,0x43,0x48},1,1,2,0),2));
   Network::DrainRelayPackets(uds.system);
   Check(Status(uds)==static_cast<unsigned>(NetworkStatus::NotConnected)&&uds.node_map.empty()&&uds.channel_data.empty()&&uds.node_info.empty(),"Old batch survived room-generation transition");
   const auto& page=uds.system.Kernel().GetSharedPageHandler().GetSharedPage();Check(std::equal(relay.local.begin(),relay.local.end(),page.wifi_macaddr),"Room transition retained old shared-page MAC");
   Check(uds.relay_generation==relay.status.generation&&uds.relay_connected&&Network::Detail::TakeRelayWork(),"Room transition was not reconciled/rearmed");
  }
  std::cout<<"PASS generation changes during Drain/first TrySend clear old guest batch and rearm\n";
  // Real mapped IPC output: short/exact/partial beacon entries preserve poison beyond bounds.
  for(unsigned size:{0u,11u,12u,51u,52u,64u,128u}){
   std::vector<u8> poison(256,0xa5);uds.system.Memory().WriteBlock(*uds.system.Kernel().GetCurrentProcess(),0x101000,poison.data(),poison.size());uds.received_beacons.clear();
   for(unsigned i=0;i<2;++i){Network::WifiPacket b;b.type=Network::WifiPacket::PacketType::Beacon;b.channel=11;b.transmitter_address=M(i+2);b.data.assign(12,0x6c);uds.received_beacons.push_back(b);}
   auto ctx=Context(uds,0x0f,16,4);std::array<u32_le,IPC::COMMAND_BUFFER_LENGTH> raw{};raw[0]=IPC::MakeHeader(0xf,16,4);raw[1]=size;raw[4]=0xffffffff;raw[5]=0xffff;raw[17]=IPC::CopyHandleDesc();raw[18]=0;raw[19]=IPC::MappedBufferDesc(size,IPC::RW);raw[20]=0x101000;
   Check(ctx->PopulateFromIncomingCommandBuffer(raw.data(),uds.system.Kernel().GetCurrentProcess()).IsSuccess(),"Mapped beacon IPC setup failed");uds.RecvBeaconBroadcastData(*ctx);
   std::vector<u8> actual(256);uds.system.Memory().ReadBlock(*uds.system.Kernel().GetCurrentProcess(),0x101000,actual.data(),actual.size());for(unsigned i=size;i<256;++i)Check(actual[i]==0xa5,"Beacon IPC wrote beyond guest output");
   if(size<12)Check(ctx->CommandBuffer()[1]!=0&&actual==poison,"Short beacon header was written");else Check(uds.system.Memory().Read32(0x101008)==(size>=92?2u:size>=52?1u:0u),"Beacon header counted entries not copied");
  }
  Check(uds.system.CoreTiming().GetGlobalTicks()==ticks,"Ingress fixture changed guest ticks");
  std::cout<<"PASS actual NWM pending/full host, sparse reuse, host authority, NodeMap-only departure, bounded queues, mapped beacon bounds and unchanged ticks\n";
 }
 static void RestoreDisconnectedSameGeneration(NWM_UDS& uds){
  const auto status=uds.relay_session->GetStatus();Check(status.state==Network::RelayClient::State::Disconnected,"Restore fixture requires disconnected host transport");
  uds.connection_status.status=static_cast<u32>(NetworkStatus::ConnectedAsClient);uds.connection_status.total_nodes=2;uds.connection_status.max_nodes=4;
  uds.system.Kernel().GetSharedPageHandler().SetMacAddress(M(99));
  uds.relay_generation=status.generation;uds.relay_connected=false;uds.relay_needs_initial_sync=true;uds.connection_status_event->Clear();Network::Detail::NotifyRelayWork();
  Network::DrainRelayPackets(uds.system);Check(Status(uds)==static_cast<unsigned>(NetworkStatus::NotConnected)&&Count(uds)==0&&StatusSignalled(uds),"Restored association survived disconnected same-generation session");
  const auto& page=uds.system.Kernel().GetSharedPageHandler().GetSharedPage();auto local_mac=uds.relay_session->GetLocalMac();Check(std::equal(local_mac.begin(),local_mac.end(),page.wifi_macaddr),"Restored shared-page MAC differs from host session");
  uds.node_map[M(2)]={true,2};Network::WifiPacket beacon;beacon.data.assign(12,0);uds.received_beacons.push_back(beacon);
  uds.relay_needs_initial_sync=true;uds.connection_status_event->Clear();Network::Detail::NotifyRelayWork();Network::DrainRelayPackets(uds.system);
  Check(uds.node_map.empty()&&uds.received_beacons.empty()&&StatusSignalled(uds),"Restored peer/beacon cache survived disconnected initial sync");
 }
};
}
int main(int argc,char** argv){@autoreleasepool{try{
 const bool offline=argc==4&&std::string(argv[3])=="--offline";if(argc!=3&&!offline)return 2;
 bool host=false;std::shared_ptr<Network::RelayClient> relay;
 if(offline){relay=std::make_shared<FixtureRelay>();}
 else {
 std::string line;std::getline(std::cin,line);Check(line.size()<=8192,"Input too large");NSData* input=[NSData dataWithBytes:line.data() length:line.size()];NSDictionary* values=[NSJSONSerialization JSONObjectWithData:input options:0 error:nil];line.clear();
 Check([values isKindOfClass:NSDictionary.class],"Missing private stdin configuration");NSString* room=values[@"room"];NSString* password=values[@"password"];NSString* role=values[@"role"];
 Check([room isKindOfClass:NSString.class]&&[password isKindOfClass:NSString.class]&&[role isKindOfClass:NSString.class],"Invalid stdin configuration");host=[role isEqualToString:@"host"];
 relay=Network::RelayClient::Create();Network::RelayClient::Config config;config.room=room.UTF8String;config.password=password.UTF8String;Check(relay->Connect(std::move(config)),"Relay start failed");
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);while(relay->GetStatus().state!=Network::RelayClient::State::Connected&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));Check(relay->GetStatus().state==Network::RelayClient::State::Connected,"Relay admission failed");values=nil;password=nil;input=nil;
 }
 Common::DetachedTasks tasks;const auto output=std::filesystem::absolute(argv[2]);std::filesystem::create_directories(output);FileUtil::SetUserPath((output/"userdata").string()+"/");
 Log::SetGlobalFilter(Log::Filter(Log::Level::Warning));Log::AddBackend(std::make_unique<Log::FileBackend>((output/"hle.log").string()));
 auto& settings=Settings::values;settings.is_new_3ds=false;settings.use_cpu_jit=false;settings.use_shader_jit=false;settings.use_hw_renderer=false;settings.use_hw_shader=false;settings.use_virtual_sd=true;settings.sink_id="null";settings.region_value=Settings::REGION_VALUE_AUTO_SELECT;settings.init_clock=Settings::InitClock::FixedTime;settings.init_time=946684801;settings.resolution_factor=1;settings.use_frame_limit=true;settings.frame_limit=100;settings.shared_font_type=Settings::SharedFontType::Auto;
 Settings::Apply();Frontend::RegisterDefaultApplets();Network::InstallRelaySession(relay);Window window;auto& system=Core::System::GetInstance();Check(system.Load(window,std::filesystem::absolute(argv[1]).string())==Core::System::ResultStatus::Success,"Generated ELF load failed");SCOPE_EXIT({relay->Disconnect();system.Shutdown();Network::InstallRelaySession(nullptr);});
 auto uds=system.ServiceManager().GetService<Service::NWM::NWM_UDS>("nwm::UDS");Check(bool(uds),"NWM service missing");using Driver=Service::NWM::RelayHLETestDriver;Driver::Initialize(*uds);Network::DrainRelayPackets(system);
 if(offline){Check(system.RunLoop()==Core::System::ResultStatus::Success,"Offline generated guest startup failed");Driver::Offline(*uds,*std::static_pointer_cast<FixtureRelay>(relay));relay->Disconnect();Network::DrainRelayPackets(system);Driver::RestoreDisconnectedSameGeneration(*uds);std::cout<<"PASS offline same-generation restored association\n";return 0;}
 std::optional<std::chrono::steady_clock::time_point> origin;s64 ticks{};
 auto pump=[&](const std::function<bool()>& condition,unsigned seconds){if(!origin){origin=std::chrono::steady_clock::now();ticks=system.CoreTiming().GetGlobalTicks();}auto until=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);while(std::chrono::steady_clock::now()<until){Network::DrainRelayPackets(system);if(condition())return true;Check(system.RunLoop()==Core::System::ResultStatus::Success,"Generated guest execution failed");const auto elapsed=system.CoreTiming().GetGlobalTicks()-ticks;const s64 ns=(elapsed/BASE_CLOCK_RATE_ARM11)*1000000000LL+(elapsed%BASE_CLOCK_RATE_ARM11)*1000000000LL/BASE_CLOCK_RATE_ARM11;std::this_thread::sleep_until(*origin+std::chrono::nanoseconds(ns));}return false;};
 if(host){
  Driver::Host(*uds);std::cout<<"READY\n"<<std::flush;Check(pump([&]{return Driver::Count(*uds)==2&&Driver::HasPacket(*uds);},20),"Host did not receive actual UDS handshake/data");Driver::Pull(*uds,Payload(false),2);Driver::Send(*uds,2,Payload(true));Driver::ClearStatusEvent(*uds);
  Check(pump([&]{return Driver::Count(*uds)==1;},10),"Host did not remove departing client");Check(Driver::Status(*uds)==static_cast<unsigned>(Service::NWM::NetworkStatus::ConnectedAsHost)&&Driver::StatusSignalled(*uds),"Host departure state/event incorrect");Check(!Driver::HasPacket(*uds),"Spoofed source entered host queue");
  std::cout<<"PASS actual host Initialize/BeginHosting/Bind/handshake/Pull/Send/departure; spoof source rejected\n";
 }else{
  std::cout<<"READY\n"<<std::flush;Service::NWM::NetworkInfo info;
  // Select the generated network using a real received beacon and its transmitter MAC.
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);bool discovered=false;while(std::chrono::steady_clock::now()<deadline){Network::DrainRelayPackets(system);if(Driver::FindHost(*uds,info)){discovered=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}Check(discovered,"No actual host beacon received");
  auto connection=Driver::Connect(*uds,info);Check(pump([&]{return Driver::Status(*uds)==static_cast<unsigned>(Service::NWM::NetworkStatus::ConnectedAsClient);},10),"Client actual UDS connection failed");Check(Driver::Node(*uds)==2,"Unexpected assigned UDS node");Driver::Bind(*uds);Driver::SpoofSource(*uds);Driver::Send(*uds,1,Payload(false));Check(pump([&]{return Driver::HasPacket(*uds);},10),"Client did not receive host UDS payload");Driver::Pull(*uds,Payload(true),1);relay->Disconnect();Network::DrainRelayPackets(system);Check(Driver::Status(*uds)==static_cast<unsigned>(Service::NWM::NetworkStatus::NotConnected),"Client disconnect left stale UDS state");
  std::cout<<"PASS actual client Initialize/Connect/Bind/Send/Pull via WSS and selected host\n";
 }
 relay->Disconnect();Network::DrainRelayPackets(system);Driver::RestoreDisconnectedSameGeneration(*uds);std::cout<<"PASS same-generation restored-association disconnect/event regression\n";
 return 0;
}catch(const std::exception& error){std::cerr<<"Service-HLE witness failed: "<<error.what()<<"\n";return 1;}}}
