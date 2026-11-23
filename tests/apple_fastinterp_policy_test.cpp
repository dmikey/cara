// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/apple_fastinterp_policy.h"
#include <cassert>
#include <initializer_list>
#include <cstring>
int main() {
    unsetenv("CARA_FASTINTERP_ALU_PAIRS");
    for(auto name:{"CARA_CPU_BACKEND","CARA_FASTINTERP_ALU_OPERANDS","CARA_FASTINTERP_TICK_BUCKETS","CARA_FASTINTERP_CHAIN_BUDGET","CARA_FASTINTERP_CACHE_WAYS","CARA_FASTINTERP_CACHE_ENTRIES","CARA_FASTINTERP_RAM_BURST","CARA_FASTINTERP_AL_DISPATCH"}) unsetenv(name);
    Common::ApplyAppleFastInterpDefaults(4ULL<<30);
    assert(!std::getenv("CARA_FASTINTERP_CACHE_ENTRIES"));
    assert(!std::strcmp(std::getenv("CARA_FASTINTERP_CACHE_WAYS"),"8"));
    assert(!std::strcmp(std::getenv("CARA_FASTINTERP_RAM_BURST"),"1"));
    assert(!std::strcmp(std::getenv("CARA_FASTINTERP_ALU_PAIRS"),"1"));
    setenv("CARA_FASTINTERP_CACHE_WAYS","4",1);
    setenv("CARA_FASTINTERP_AL_DISPATCH","0",1);
    Common::ApplyAppleFastInterpDefaults(8ULL<<30);
    assert(!std::strcmp(std::getenv("CARA_FASTINTERP_CACHE_ENTRIES"),"32768"));
    assert(!std::strcmp(std::getenv("CARA_FASTINTERP_CACHE_WAYS"),"4"));
    assert(!std::strcmp(std::getenv("CARA_FASTINTERP_AL_DISPATCH"),"0"));
    for (const auto* value : {"0", "", "custom"}) {
        setenv("CARA_FASTINTERP_ALU_PAIRS",value,1);
        Common::ApplyAppleFastInterpDefaults(8ULL<<30);
        assert(!std::strcmp(std::getenv("CARA_FASTINTERP_ALU_PAIRS"),value));
    }
}
