#pragma once
#include "veyra/pipeline/ResolutionPlan.h"
#include <algorithm>
#include <cmath>

namespace veyra::engine {
// A runtime projection of browser COMPOSITED pixels, not a decoder-texture
// handoff. Empty means keep every captured pixel. The original-video metadata
// may only reduce work when both images describe the same displayed aspect.
inline bool browserRtxInputAspectMatches(pipeline::Extent a,pipeline::Extent b) {
    if(!a.valid()||!b.valid())return false;
    const auto left=double(a.width)*b.height,right=double(b.width)*a.height;
    return std::abs(left-right)<=std::max(left,right)*0.005;
}
inline pipeline::Extent browserSrInputExtent(pipeline::Extent capture,pipeline::Extent original) {
    if(!capture.valid()||!original.valid()||capture.width<64||capture.height<64||
       original.width<64||original.height<64||!browserRtxInputAspectMatches(capture,original))return {};
    const double scale=std::min({1.0,double(original.width)/capture.width,
        double(original.height)/capture.height});
    if(scale>=1.0)return {};
    const pipeline::Extent input{uint32_t(capture.width*scale+1e-6)&~1u,
        uint32_t(capture.height*scale+1e-6)&~1u};
    if(input.width<64||input.height<64||input.width>original.width||input.height>original.height||
       input.width>capture.width||input.height>capture.height||input==capture||
       !browserRtxInputAspectMatches(input,capture))return {};
    return input;
}
// RTX's existing performance/full-capture switches apply only to RTX. DLSS
// always reconstructs from the reported source-sized sampling grid when safe.
inline pipeline::Extent browserSrInputExtent(pipeline::Extent capture,pipeline::Extent original,
                                             uint32_t quality,bool rtxSourceSized,bool rtxFullCapture) {
    if(quality!=0&&quality!=5&&!(quality>=1&&quality<=4&&rtxSourceSized&&!rtxFullCapture))return {};
    return browserSrInputExtent(capture,original);
}
// Kept for existing callers/tests; no settings or serialized field is renamed.
inline pipeline::Extent browserRtxInputExtent(pipeline::Extent capture,pipeline::Extent original) {
    return browserSrInputExtent(capture,original);
}
// Independent graph-side validation: stale, malformed or unrelated metadata
// must never enlarge/reshape the input, affect an output extent, or request a
// non-upscale operation from an SR backend.
inline pipeline::Extent validatedVideoSrInputExtent(pipeline::Extent hint,pipeline::Extent capture,
                                                    pipeline::Extent output) {
    if(hint==pipeline::Extent{}||!hint.valid()||!capture.valid()||!output.valid()||
       hint.width<64||hint.height<64||hint.width>capture.width||hint.height>capture.height||
       hint.width>output.width||hint.height>output.height||hint==capture||hint==output||
       !browserRtxInputAspectMatches(hint,capture))return {};
    return hint;
}
}
