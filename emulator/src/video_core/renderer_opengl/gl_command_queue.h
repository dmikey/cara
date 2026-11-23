// Staged single-context GL driver-dispatch queue. C++ render metadata stays producer-owned.
#pragma once
#include <cstddef>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <glad/glad.h>
#include "gl_command_queue_api.h"
#ifdef CARA_GL_CONTEXT_DISPATCH
#include "gl_driver_transactions.h"
#endif
#include "gl_transaction_attribution.h"
#include "common/host_render_queue.h"
namespace Common { class HostRenderQueue; }
namespace OpenGL { class SharedVertexShadow; }
namespace OpenGL::CommandQueue {
struct Stats {
    std::uint64_t owned_native_map_uploads=0,transaction_inline_calls=0,transaction_sync_calls=0,transaction_state_fallbacks=0,transaction_lut_calls=0,owned_unmap_error_queries=0,owned_budget_live=0,owned_budget_high=0,owned_budget_waits=0;
    std::array<std::uint64_t,RpcApiCount> rpc_by_api{};
    std::uint64_t uniform_location_rpcs=0,uniform_location_cache_hits=0,worker_rejected_calls=0;
    std::uint64_t attrib_location_cache_hits=0,uniform_block_index_cache_hits=0;
    std::uint64_t recorded_commands=0, submitted_batches=0, synchronous_calls=0;
    std::uint64_t owned_payload_bytes=0, conservative_pointer_rpcs=0, texture_pointer_rpcs=0;
    std::uint64_t texture_owned_uploads=0, texture_pbo_offsets=0, client_array_rpcs=0;
    std::uint64_t pending_commands=0, pending_bytes=0, worker_inline_calls=0;
};
void Install(Common::HostRenderQueue& queue);
bool ForwardsGetError(PFNGLGETERRORPROC expected);
std::shared_ptr<void> ReserveStreamPayload();
void Uninstall();
bool Enabled();
// Separate diagnostic accessor; preserves the existing Stats ABI.
struct UncheckedBlitProducerStats {
    bool requested=false, active=false;
    // Required blit API commands recorded, not GPU completion or scope-exit success.
    std::uint64_t blit_commands_recorded=0;
};
bool UncheckedBlitProducerRequested();
UncheckedBlitProducerStats GetUncheckedBlitProducerStats();
bool NativeMapUploadRequested();
bool NativeMapUploadActive();
Common::HostRenderQueue* Queue();
#ifdef CARA_GL_CONTEXT_DISPATCH
#ifdef __ANDROID__
// Durations of producer-side synchronous requests during an enabled frame profile.
bool TryQueueVertexUpload(GLsizei width, GLsizei height, GLenum format, GLenum type,
                          const void* pixels, std::size_t bytes, SharedVertexShadow& retained);
struct RemapState;
std::shared_ptr<RemapState> SaveRemapStateIfEnabled();
void RestoreRemapState(std::shared_ptr<RemapState> state);
bool CheckedRemapDraw(std::function<bool()> draw);

struct SyncTiming {
    std::uint64_t count{}, before_driver_ns{}, in_driver_ns{}, after_driver_ns{};
};
std::array<SyncTiming, RpcApiCount> TakeSyncTimingsForProfile();
#endif
void QueueLUT(DriverTransactions::LUTBody body, GLuint texture, unsigned unit, GLenum format,
              std::size_t offset, const std::uint8_t* data, std::size_t bytes);
#endif
void Flush();
void Drain();
void SubmitHost(std::function<void()> job,std::size_t bytes=0);
void UploadBufferOwned(GLenum target,GLintptr offset,std::vector<std::uint8_t> bytes,
                       bool orphan,GLsizeiptr fullsize,bool check_error=false,std::shared_ptr<void> reservation={},GLsizeiptr map_reserved_size=0);
Stats SnapshotStats();
inline Stats GetStats() { return SnapshotStats(); }
void ResetStats();
// Producer-only cumulative diagnostic snapshot; no drain/query/reset here.
TransactionAttribution::Snapshot GetTransactionAttribution();
// Diagnostic profile boundaries only, never the rendering hot path.
std::uint64_t ReadWorkerCPUForProfile();
} // namespace OpenGL::CommandQueue
