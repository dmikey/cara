// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/apple_emulation_policy.h"
#include <cassert>
#include <thread>
int main() {
    // Dedicated test threads end after each policy application; no process-wide
    // scheduler, power settings, affinity, or realtime policy is changed.
    for(auto initial:{QOS_CLASS_BACKGROUND,QOS_CLASS_UTILITY,QOS_CLASS_DEFAULT,QOS_CLASS_USER_INITIATED,QOS_CLASS_USER_INTERACTIVE}) {
        std::thread thread([initial] {
            assert(pthread_set_qos_class_self_np(initial,0)==0);
            auto policy=Common::ApplyAppleEmulationQoS();
            assert(policy.result==0);
            assert(policy.initial==initial);
            assert(policy.requested==(initial==QOS_CLASS_USER_INTERACTIVE?QOS_CLASS_USER_INTERACTIVE:QOS_CLASS_USER_INITIATED));
            assert(policy.effective==policy.requested);
        });
        thread.join();
    }
}
