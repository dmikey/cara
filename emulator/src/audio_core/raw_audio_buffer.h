// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include "common/common_types.h"

namespace AudioCore {
// A short source reserve absorbs guest frame pacing jitter. Fades prevent a
// missing source block from holding a DC sample then jumping on recovery.
class RawAudioBuffer {
public:
    static constexpr std::size_t prefill_frames=2048;
    bool Ready(std::size_t available,std::size_t requested) {
        if (prefilling && available<std::max(prefill_frames,requested)) return false;
        prefilling=false; return true;
    }
    void Reset() { prefilling=true; fade_in=true; }
    void Finish(s16* samples,std::size_t supplied,std::size_t requested) {
        if (fade_in && supplied) {
            const auto count=std::min<std::size_t>(64,supplied);
            for (std::size_t i=0;i<count;++i) for(unsigned ch=0;ch<2;++ch)
                samples[2*i+ch]=s16(s32(samples[2*i+ch])*s32(i)/s32(std::max<std::size_t>(1,count-1)));
            fade_in=false;
        }
        if (supplied<requested) {
            const auto count=std::min<std::size_t>(64,supplied);
            for (std::size_t i=0;i<count;++i) for(unsigned ch=0;ch<2;++ch)
                samples[2*(supplied-count+i)+ch]=s16(s32(samples[2*(supplied-count+i)+ch])*s32(count-1-i)/s32(std::max<std::size_t>(1,count-1)));
            std::fill(samples+2*supplied,samples+2*requested,s16(0));
            if (supplied==0) {
                const auto fade=std::min<std::size_t>(64,requested);
                for (std::size_t i=0;i<fade;++i) for(unsigned ch=0;ch<2;++ch)
                    samples[2*i+ch]=s16(s32(previous[ch])*s32(fade-1-i)/s32(std::max<std::size_t>(1,fade-1)));
            }
            Reset();
        }
        if(requested) previous={samples[2*(requested-1)],samples[2*(requested-1)+1]};
    }
private:
    bool prefilling=true,fade_in=true;
    std::array<s16,2> previous{};
};
} // namespace AudioCore
