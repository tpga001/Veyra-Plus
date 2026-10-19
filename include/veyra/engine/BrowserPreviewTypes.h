#pragma once
#include <cstdint>
#include <cmath>
#include <string>
namespace veyra::engine {
// In-process only. Never serialized into the browser HTTP protocol or presets.
struct BrowserPreviewRequest {
    uintptr_t window=0;
    uint64_t sessionId=0,revision=0;
    bool visible=false;
    // Secondary monitor presentation only. Never the primary comparisonMode.
    int comparisonMode=0; // 0 enhanced, 1 original, 2 left-original wipe
    float comparisonSplit=.5f;
    bool operator==(const BrowserPreviewRequest&)const=default;
};
enum class BrowserPreviewState { Disabled,Waiting,Ready,Frozen,Busy,Unsupported,Unavailable,Failed };
struct BrowserPreviewSnapshot {
    uintptr_t window=0;
    uint64_t sessionId=0,revision=0;
    bool hasFrame=false,live=false,failed=false;
    BrowserPreviewState state=BrowserPreviewState::Disabled;
    std::wstring status=L"预览未开启";
    uint64_t submitted=0,gpuCompleted=0,skippedBusy=0;
    uint32_t width=0,height=0;
    uint64_t sourceFrameId=0,epoch=0,settingsRevision=0,generation=0;
    // Actual successfully presented and GPU-completed image, not UI requests.
    int comparisonMode=0;
    float comparisonSplit=.5f;
    bool referenceAvailable=false;
    uint64_t frameRequestRevision=0;
    bool operator==(const BrowserPreviewSnapshot&)const=default;
};
inline bool browserPreviewComparisonValid(const BrowserPreviewRequest& request) {
    return request.comparisonMode>=0&&request.comparisonMode<=2&&std::isfinite(request.comparisonSplit)&&
        request.comparisonSplit>=0.0f&&request.comparisonSplit<=1.0f;
}
inline bool browserPreviewRequestMatches(const BrowserPreviewRequest& request,uint64_t sessionId) {
    return request.window&&request.visible&&request.revision&&request.sessionId&&request.sessionId==sessionId&&
        browserPreviewComparisonValid(request);
}
inline bool browserPreviewNeedsReferences(const BrowserPreviewRequest& request,uint64_t sessionId) {
    return request.comparisonMode!=0&&browserPreviewRequestMatches(request,sessionId);
}
// Replaying a request may not change its identity. A new engine session needs
// a fresh request; an old session is never a reusable HWND-only authority.
inline bool browserPreviewRevisionAccepted(const BrowserPreviewRequest& previous,const BrowserPreviewRequest& next) {
    if(!next.revision||!browserPreviewComparisonValid(next))return false;
    if(next.revision<previous.revision)return false;
    return next.revision!=previous.revision||next==previous;
}
inline bool browserPreviewCompletionMatches(const BrowserPreviewRequest& requested,
                                            const BrowserPreviewRequest& completed) {
    return requested.window&&requested.sessionId&&requested.revision&&
        requested.window==completed.window&&requested.sessionId==completed.sessionId&&
        requested.revision==completed.revision&&requested.comparisonMode==completed.comparisonMode&&
        requested.comparisonSplit==completed.comparisonSplit&&browserPreviewComparisonValid(requested);
}
inline bool browserPreviewGenerationMatches(uint64_t currentEpoch,uint64_t currentRevision,uint64_t firstSourceFrameId,
                                           uint64_t frameEpoch,uint64_t frameRevision,uint64_t frameSourceId) {
    return currentRevision&&currentEpoch==frameEpoch&&currentRevision==frameRevision&&
        frameSourceId>=firstSourceFrameId;
}
inline bool browserPreviewFrameReady(const BrowserPreviewRequest& requested,const BrowserPreviewRequest& completed,
                                     uint64_t completedFence,uint64_t requiredFence,bool presented) {
    return presented&&requiredFence&&completedFence!=UINT64_MAX&&completedFence>=requiredFence&&
        browserPreviewCompletionMatches(requested,completed);
}
} // namespace veyra::engine
