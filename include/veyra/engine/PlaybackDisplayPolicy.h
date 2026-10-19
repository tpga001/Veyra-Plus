#pragma once
#include "veyra/engine/EnhancementSettings.h"
#include <cmath>

namespace veyra::engine {
// Physical pixels, independent of Windows DPI and taskbar working area.
// Bound cost at the existing 4K profile while retaining portrait/ultrawide shape.
inline pipeline::Extent boundedPlaybackDisplayExtent(pipeline::Extent display) {
    if(!display.valid()||display.width<2||display.height<2)return {};
    const double scale=std::min({1.0,3840.0/std::max(display.width,display.height),
        2160.0/std::min(display.width,display.height)});
    return {std::max(2u,uint32_t(display.width*scale)&~1u),
            std::max(2u,uint32_t(display.height*scale)&~1u)};
}
// The saved target remains manual. Only the copy sent to live video gets a limit.
inline EnhancementSettings displayPlaybackSettings(const EnhancementSettings& manual,
    pipeline::Extent display,bool follow,bool listVideo) {
    auto playback=manual;
    playback.srPlaybackExtent=follow&&listVideo&&manual.sr?boundedPlaybackDisplayExtent(display):pipeline::Extent{};
    return playback;
}
}
