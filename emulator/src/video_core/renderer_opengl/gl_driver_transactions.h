// Diagnostic host-driver bridge. Null hooks preserve the direct path.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
namespace OpenGL::DriverTransactions {
enum class Kind : unsigned { Blit, Remap, DepthEncode, DepthUpload, LUT, Count };
using LUTBody=void(*)(std::uint32_t,unsigned,std::uint32_t,std::size_t,const std::uint8_t*,std::size_t);
struct Hooks {
 bool (*sync_bool)(Kind,const std::function<bool()>&);
 void (*sync_void)(Kind,const std::function<void()>&);
 void (*lut)(LUTBody,std::uint32_t,unsigned,std::uint32_t,std::size_t,const std::uint8_t*,std::size_t);
};
inline std::atomic<const Hooks*> hooks{nullptr};
// Optional diagnostic admission on the sole producer. Existing Hooks layout and
// normal synchronous bridge are unchanged. No helper closure is queued here.
using UncheckedBlitProducerGate = bool(*)();
inline std::atomic<UncheckedBlitProducerGate> unchecked_blit_producer_gate{nullptr};
inline std::atomic<std::uint64_t> unchecked_blit_commands_recorded{0};
inline bool TryUncheckedBlitProducer() {
    const auto gate = unchecked_blit_producer_gate.load(std::memory_order_acquire);
    return gate && gate();
}
// Lexical worker token; only the installed driver backend establishes it.
inline thread_local unsigned execution_depth=0;
struct ExecutionScope { ExecutionScope(){++execution_depth;} ~ExecutionScope(){--execution_depth;} ExecutionScope(const ExecutionScope&)=delete; };
inline bool Executing(){return execution_depth!=0;}
inline const Hooks* Pending(){return Executing()?nullptr:hooks.load(std::memory_order_acquire);}
}
