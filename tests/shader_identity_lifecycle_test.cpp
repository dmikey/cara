// SPDX-License-Identifier: GPL-2.0-or-later
// Actual ShaderSetup identity and archive lifecycle regression.
#include "video_core/shader/shader.h"
#include "video_core/shader/shader_identity.hh"
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
using Pica::Shader::ShaderSetup;
void Check(bool value,const char* reason) { if(!value)throw std::runtime_error(reason); }
std::string Save(const ShaderSetup& setup) {
    std::ostringstream bytes(std::ios::binary);{boost::archive::binary_oarchive ar(bytes);ar<<setup;}return bytes.str();
}
void Load(ShaderSetup& setup,const std::string& bytes) {
    std::istringstream in(bytes,std::ios::binary);boost::archive::binary_iarchive ar(in);ar>>setup;
}
int main() { try {
    ShaderSetup a{};a.program_code[7]=0x12345678;a.MarkProgramCodeDirty();
    a.swizzle_data[11]=0x87654321;a.MarkSwizzleDataDirty();
    const auto code_hash=a.GetProgramCodeHash(),swizzle_hash=a.GetSwizzleDataHash();
    const auto archive=Save(a);
    const auto c=a.GetProgramCodeIdentity(),s=a.GetSwizzleDataIdentity();
    Check(c && s && c!=s,"Lazy tickets must be nonzero and globally distinct");
    Check(a.GetProgramCodeIdentity()==c && a.GetSwizzleDataIdentity()==s,"Stable shader identity changed");
    Check(Save(a)==archive,"Identity allocation changed serialized bytes");
    auto copied=a;
    Check(copied.GetProgramCodeIdentity()==c && copied.GetSwizzleDataIdentity()==s,"Exact copy lost valid identity");
    copied.program_code[7]^=1;copied.MarkProgramCodeDirty();
    Check(copied.GetProgramCodeIdentity()!=c && copied.GetSwizzleDataIdentity()==s,"Independent code invalidation failed");
    Check(a.GetProgramCodeIdentity()==c && a.program_code[7]==0x12345678,"Mutated copy changed original lease");
    copied.swizzle_data[11]^=1;copied.MarkSwizzleDataDirty();
    Check(copied.GetSwizzleDataIdentity()!=s,"Swizzle invalidation failed");
    const auto before_load_code=copied.GetProgramCodeIdentity(),before_load_swizzle=copied.GetSwizzleDataIdentity();
    Load(copied,archive);
    Check(copied.program_code==a.program_code && copied.swizzle_data==a.swizzle_data,"Archive changed shader arrays");
    Check(copied.GetProgramCodeIdentity()!=before_load_code && copied.GetSwizzleDataIdentity()!=before_load_swizzle,
          "Loaded state retained previous runtime identity");
    Check(copied.GetProgramCodeHash()==code_hash && copied.GetSwizzleDataHash()==swizzle_hash,"Restored hash fields changed");
    Check(Save(copied)==archive,"Load altered original archive field sequence/values");
    const auto partial_code=copied.GetProgramCodeIdentity(),partial_swizzle=copied.GetSwizzleDataIdentity();
    bool rejected=false;try {Load(copied,archive.substr(0,archive.size()/2));}catch(...){rejected=true;}
    Check(rejected,"Truncated archive accepted");
    Check(copied.GetProgramCodeIdentity()!=partial_code && copied.GetSwizzleDataIdentity()!=partial_swizzle,
          "Partial load retained pre-load identity");
    static_assert(std::is_trivially_copyable_v<ShaderSetup>);
    const auto prior_reset=copied.GetProgramCodeIdentity();
    std::memset(&copied,0,sizeof(copied));
    Check(copied.GetProgramCodeIdentity()!=prior_reset,"Raw State::Zero reused identity");
    std::atomic<std::uint64_t> tickets{1};
    std::vector<std::uint64_t> values(8*64);std::vector<std::thread> workers;
    for(unsigned t=0;t<8;++t)workers.emplace_back([&,t]{for(unsigned i=0;i<64;++i)values[t*64+i]=Pica::Shader::Detail::AllocateIdentity(tickets);});
    for(auto& thread:workers)thread.join();
    Check(std::set<std::uint64_t>(values.begin(),values.end()).size()==values.size(),"Ticket allocator repeated an identity");
    std::atomic<std::uint64_t> last{std::numeric_limits<std::uint64_t>::max()-1};
    Check(Pica::Shader::Detail::AllocateIdentity(last)==std::numeric_limits<std::uint64_t>::max()-1,"Final ticket wrong");
    for(unsigned i=0;i<16;++i)Check(Pica::Shader::Detail::AllocateIdentity(last)==0,"Saturated allocator wrapped");
    Check(last.load()==std::numeric_limits<std::uint64_t>::max(),"Saturated allocator reset its sentinel");
    std::cout<<"PASS lazy IDs, exact copies, independent mutations, archive byte equality, restore/partial-load/reset, concurrent allocator and saturation\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;} }
