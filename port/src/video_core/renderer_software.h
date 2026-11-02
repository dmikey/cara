// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_base.h"

namespace VideoCore {
// CPU framebuffer presentation for platforms without the required OpenGL feature level.
class RendererSoftware final : public RendererBase {
public:
    using RendererBase::RendererBase;
    ResultStatus Init() override { return ResultStatus::Success; }
    void SwapBuffers() override;
    bool TryPresent() override { return true; }
    void ResetPresent() override {}
};
}
