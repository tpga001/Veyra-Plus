#pragma once
#include "veyra/engine/EngineController.h"
#include <cmath>

namespace veyra::ui {
inline bool sameTelemetryNumber(double a, double b) {
    return a == b || (std::isnan(a) && std::isnan(b));
}
inline bool sameTelemetryNumber(const std::optional<double>& a, const std::optional<double>& b) {
    return a.has_value() == b.has_value() && (!a || sameTelemetryNumber(*a, *b));
}
inline bool sameTimingAggregate(const diagnostics::TimingAggregate& a, const diagnostics::TimingAggregate& b) {
    return a.samples == b.samples && sameTelemetryNumber(a.mean, b.mean) && sameTelemetryNumber(a.p95, b.p95);
}
inline bool sameFlowTelemetry(const diagnostics::FrameFlowMetrics& a, const diagnostics::FrameFlowMetrics& b) {
#define VEYRA_FLOW_EQUAL(member) if (a.member != b.member) return false
#define VEYRA_FLOW_NUMBER(member) if (!sameTelemetryNumber(a.member, b.member)) return false
    VEYRA_FLOW_EQUAL(lastReady100ns); VEYRA_FLOW_EQUAL(lastPresent100ns); VEYRA_FLOW_EQUAL(lastSubmit100ns);
    VEYRA_FLOW_EQUAL(lastFgRejected100ns); VEYRA_FLOW_EQUAL(latest.sessionId); VEYRA_FLOW_EQUAL(latest.frame);
    VEYRA_FLOW_EQUAL(latest.batchId); VEYRA_FLOW_EQUAL(latest.readyFence); VEYRA_FLOW_EQUAL(latest.consumerFence);
    VEYRA_FLOW_EQUAL(counters.captureReceived); VEYRA_FLOW_EQUAL(counters.mailboxOverwritten);
    VEYRA_FLOW_EQUAL(counters.sourceAccepted); VEYRA_FLOW_EQUAL(counters.sourceSkippedBeforeGraph);
    VEYRA_FLOW_EQUAL(counters.previewSkippedBeforeGraph); VEYRA_FLOW_EQUAL(counters.realSubmitted);
    VEYRA_FLOW_EQUAL(counters.fgCandidate); VEYRA_FLOW_EQUAL(counters.fgEvaluated); VEYRA_FLOW_EQUAL(counters.fgSkippedBeforeEval);
    VEYRA_FLOW_EQUAL(counters.fgReadyValid); VEYRA_FLOW_EQUAL(counters.fgInvalid); VEYRA_FLOW_EQUAL(counters.fgWarmup);
    VEYRA_FLOW_EQUAL(counters.fgSkippedForReset); VEYRA_FLOW_EQUAL(counters.fgReduced);
    VEYRA_FLOW_EQUAL(counters.xessSdkGenerated); VEYRA_FLOW_EQUAL(counters.xessSdkPresented); VEYRA_FLOW_EQUAL(counters.realReady);
    VEYRA_FLOW_EQUAL(counters.realPresented); VEYRA_FLOW_EQUAL(counters.generatedPresented);
    VEYRA_FLOW_EQUAL(counters.generatedExpiredAfterEval); VEYRA_FLOW_EQUAL(counters.cancelledBeforePresent);
    VEYRA_FLOW_EQUAL(counters.historyResets); VEYRA_FLOW_EQUAL(counters.captureDropResets); VEYRA_FLOW_EQUAL(counters.settingsResets);
    VEYRA_FLOW_EQUAL(counters.commandSlotsInFlight); VEYRA_FLOW_EQUAL(counters.commandSlotHighWater);
    VEYRA_FLOW_EQUAL(counters.presentationBatchHighWater); VEYRA_FLOW_EQUAL(pendingOutputFrames);
    VEYRA_FLOW_EQUAL(rateWindowReady); VEYRA_FLOW_EQUAL(slotReuseWaitCount); VEYRA_FLOW_EQUAL(latencySamples);
    VEYRA_FLOW_EQUAL(pairSourceA); VEYRA_FLOW_EQUAL(pairSourceB); VEYRA_FLOW_EQUAL(pairCaptureCallbacks); VEYRA_FLOW_EQUAL(timingOverflow);
    VEYRA_FLOW_NUMBER(slotReuseWaitMs); VEYRA_FLOW_NUMBER(captureArrivalToPresentReturnMs);
    VEYRA_FLOW_NUMBER(gpuReadyWaitMs); VEYRA_FLOW_NUMBER(deadlineWaitMs); VEYRA_FLOW_NUMBER(validGeneratedFps);
    VEYRA_FLOW_NUMBER(presentSubmitFps); VEYRA_FLOW_NUMBER(sourceCompletedFps); VEYRA_FLOW_NUMBER(outputCompletedFps);
    VEYRA_FLOW_NUMBER(xessSdkSubmitFps); VEYRA_FLOW_NUMBER(realPresentFps); VEYRA_FLOW_NUMBER(generatedPresentFps);
    VEYRA_FLOW_NUMBER(softwareLatencyMs); VEYRA_FLOW_NUMBER(softwareLatencyP95Ms); VEYRA_FLOW_NUMBER(slotWaitPerFrameMs);
    for (size_t i = 0; i < a.gpuTiming.size(); ++i) if (!sameTimingAggregate(a.gpuTiming[i], b.gpuTiming[i])) return false;
    for (size_t i = 0; i < a.cpuTiming.size(); ++i) if (!sameTimingAggregate(a.cpuTiming[i], b.cpuTiming[i])) return false;
    for (size_t i = 0; i < a.pairTiming.size(); ++i) if (!sameTimingAggregate(a.pairTiming[i], b.pairTiming[i])) return false;
    if (!sameTimingAggregate(a.enhancementProcessing, b.enhancementProcessing)) return false;
    VEYRA_FLOW_EQUAL(reset.sessionId); VEYRA_FLOW_EQUAL(reset.settingsRevision); VEYRA_FLOW_EQUAL(reset.epoch);
    VEYRA_FLOW_EQUAL(reset.sourceFrameId); VEYRA_FLOW_EQUAL(reset.reason); VEYRA_FLOW_EQUAL(reset.rebuilt); VEYRA_FLOW_EQUAL(reset.outcome);
    for (size_t i = 0; i < a.reset.stageMs.size(); ++i) if (!sameTelemetryNumber(a.reset.stageMs[i], b.reset.stageMs[i])) return false;
    VEYRA_FLOW_NUMBER(reset.totalMs);
#undef VEYRA_FLOW_EQUAL
#undef VEYRA_FLOW_NUMBER
    return true;
}
// The actual facade publication predicate, shared with CPU regression tests.
// Timing/rate changes must publish even when transport and frame counters stay
// fixed (asynchronous presents and expired timing windows can do exactly that).
inline bool samePlayerSnapshot(const engine::PlayerSnapshot& a, const engine::PlayerSnapshot& b) {
#define VEYRA_SNAPSHOT_EQUAL(member) if (a.member != b.member) return false
#define VEYRA_SNAPSHOT_NUMBER(member) if (!sameTelemetryNumber(a.member, b.member)) return false
    VEYRA_SNAPSHOT_EQUAL(transport); VEYRA_SNAPSHOT_EQUAL(sessionId); VEYRA_SNAPSHOT_EQUAL(position); VEYRA_SNAPSHOT_EQUAL(duration);
    VEYRA_SNAPSHOT_EQUAL(browserPreview);
    VEYRA_SNAPSHOT_EQUAL(outputSwapchainAddress); VEYRA_SNAPSHOT_EQUAL(outputGeneration);
    VEYRA_SNAPSHOT_EQUAL(frames); VEYRA_SNAPSHOT_EQUAL(generated); VEYRA_SNAPSHOT_EQUAL(status); VEYRA_SNAPSHOT_EQUAL(backendWarning);
    VEYRA_SNAPSHOT_EQUAL(sourceNotice); VEYRA_SNAPSHOT_EQUAL(applying); VEYRA_SNAPSHOT_EQUAL(rejectedRevision);
    VEYRA_SNAPSHOT_EQUAL(desired); VEYRA_SNAPSHOT_EQUAL(applied); VEYRA_SNAPSHOT_EQUAL(nrActive); VEYRA_SNAPSHOT_EQUAL(srActive);
    VEYRA_SNAPSHOT_EQUAL(fgActive); VEYRA_SNAPSHOT_EQUAL(failed); VEYRA_SNAPSHOT_EQUAL(volume); VEYRA_SNAPSHOT_EQUAL(muted);
    VEYRA_SNAPSHOT_EQUAL(selectedAudioTrack); VEYRA_SNAPSHOT_EQUAL(audioTracks.size()); VEYRA_SNAPSHOT_EQUAL(running);
    VEYRA_SNAPSHOT_EQUAL(sourceWidth); VEYRA_SNAPSHOT_EQUAL(sourceHeight); VEYRA_SNAPSHOT_EQUAL(sourceDisplayAspect);
    VEYRA_SNAPSHOT_EQUAL(sourceRotationDegrees); VEYRA_SNAPSHOT_EQUAL(capture); VEYRA_SNAPSHOT_EQUAL(image);
    VEYRA_SNAPSHOT_EQUAL(playbackRate); VEYRA_SNAPSHOT_EQUAL(vramRunawayMiB); VEYRA_SNAPSHOT_EQUAL(vramFullscreenUnsafe);
    VEYRA_SNAPSHOT_EQUAL(metrics.resolution.source); VEYRA_SNAPSHOT_EQUAL(metrics.resolution.base);
    VEYRA_SNAPSHOT_EQUAL(metrics.resolution.nr); VEYRA_SNAPSHOT_EQUAL(metrics.resolution.flow); VEYRA_SNAPSHOT_EQUAL(metrics.resolution.fg);
    VEYRA_SNAPSHOT_EQUAL(metrics.resolution.output); VEYRA_SNAPSHOT_EQUAL(metrics.resolution.srApplied); VEYRA_SNAPSHOT_EQUAL(metrics.resolution.settingsRevision);
    VEYRA_SNAPSHOT_EQUAL(nrAutoActive); VEYRA_SNAPSHOT_EQUAL(nrAutoPercent); VEYRA_SNAPSHOT_EQUAL(nrAutoWidth); VEYRA_SNAPSHOT_EQUAL(nrAutoHeight);
    VEYRA_SNAPSHOT_EQUAL(nrAutoStatus); VEYRA_SNAPSHOT_EQUAL(videoHdrActive); VEYRA_SNAPSHOT_EQUAL(sourceHdr);
    VEYRA_SNAPSHOT_EQUAL(colorStatus); VEYRA_SNAPSHOT_EQUAL(videoHdrStatus); VEYRA_SNAPSHOT_EQUAL(flowPerf); VEYRA_SNAPSHOT_EQUAL(contentFps);
    VEYRA_SNAPSHOT_EQUAL(captureReceived); VEYRA_SNAPSHOT_EQUAL(captureDropped); VEYRA_SNAPSHOT_EQUAL(captureRateSkipped);
    VEYRA_SNAPSHOT_EQUAL(captureHalfRate); VEYRA_SNAPSHOT_EQUAL(captureSampledBlack); VEYRA_SNAPSHOT_EQUAL(captureRecovering);
    VEYRA_SNAPSHOT_EQUAL(captureReconnectAttempts); VEYRA_SNAPSHOT_EQUAL(nrEvaluated); VEYRA_SNAPSHOT_EQUAL(nvofExecuted);
    VEYRA_SNAPSHOT_EQUAL(processedCompleted); VEYRA_SNAPSHOT_EQUAL(previewSkipped); VEYRA_SNAPSHOT_EQUAL(fgBudgetLimited);
    VEYRA_SNAPSHOT_EQUAL(xessGenerationSuppressed); VEYRA_SNAPSHOT_EQUAL(presentationProviderOwned);
    VEYRA_SNAPSHOT_EQUAL(presentationRevision); VEYRA_SNAPSHOT_EQUAL(presentationStatus); VEYRA_SNAPSHOT_EQUAL(presentationEffective);
    VEYRA_SNAPSHOT_EQUAL(fgMultiFrameMax); VEYRA_SNAPSHOT_EQUAL(fgCapabilityKnown); VEYRA_SNAPSHOT_EQUAL(xessMaxInterpolatedFrames);
    VEYRA_SNAPSHOT_EQUAL(fsrMaxGeneratedFrames); VEYRA_SNAPSHOT_EQUAL(fsrProviderVersion);
    VEYRA_SNAPSHOT_NUMBER(fps); VEYRA_SNAPSHOT_NUMBER(nominalSourceFps); VEYRA_SNAPSHOT_NUMBER(submissionFps);
    VEYRA_SNAPSHOT_NUMBER(lateMs); VEYRA_SNAPSHOT_NUMBER(lateP95Ms); VEYRA_SNAPSHOT_NUMBER(captureFps);
    VEYRA_SNAPSHOT_NUMBER(captureReadAgeMs); VEYRA_SNAPSHOT_NUMBER(captureAgeMs); VEYRA_SNAPSHOT_NUMBER(captureAgeP95Ms);
    VEYRA_SNAPSHOT_NUMBER(schedulingWaitP95Ms); VEYRA_SNAPSHOT_NUMBER(processCpuP95Ms); VEYRA_SNAPSHOT_NUMBER(presentCpuP95Ms);
    VEYRA_SNAPSHOT_NUMBER(playbackSpeed);
#undef VEYRA_SNAPSHOT_EQUAL
#undef VEYRA_SNAPSHOT_NUMBER
    return sameFlowTelemetry(a.metrics.flow, b.metrics.flow);
}
} // namespace veyra::ui
