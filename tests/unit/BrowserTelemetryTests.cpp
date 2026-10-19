#include "veyra/capture/BrowserTelemetry.h"
#include "veyra/ui/PlayerSnapshotComparison.h"
#include <iostream>
#include <limits>

int main() {
    using namespace veyra;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool pass, const char* name) {
        ++checks; if (!pass) { ++failures; std::cerr << "FAIL " << name << '\n'; }
    };
    const engine::PlayerSnapshot unchanged;
    check(ui::samePlayerSnapshot(unchanged, unchanged), "unchanged snapshots do not republish");
    const auto metricOnly = [&](const char* name, auto mutate) {
        auto changed = unchanged; mutate(changed);
        check(changed.frames == unchanged.frames && changed.transport == unchanged.transport &&
            !ui::samePlayerSnapshot(unchanged, changed), name);
    };
    metricOnly("GPU timing alone publishes", [](auto& s) { s.metrics.flow.gpuTiming[size_t(diagnostics::GpuStage::Sr)] = {2.0, 3.0, 4}; });
    metricOnly("CPU timing alone publishes without pretending to be GPU", [](auto& s) { s.metrics.flow.cpuTiming[size_t(diagnostics::CpuStage::Present)] = {4.0, 5.0, 6}; });
    metricOnly("enhancement aggregate alone publishes", [](auto& s) { s.metrics.flow.enhancementProcessing = {7.0, 8.0, 9}; });
    metricOnly("asynchronous presentation alone publishes", [](auto& s) { s.metrics.flow.counters.realPresented = 1; });
    metricOnly("provider rate alone publishes", [](auto& s) { s.metrics.flow.xessSdkSubmitFps = 119.88; });
    metricOnly("rate readiness alone publishes", [](auto& s) { s.metrics.flow.rateWindowReady = true; });
    metricOnly("queue backlog alone publishes", [](auto& s) { s.metrics.flow.pendingOutputFrames = 2; });
    metricOnly("submit FPS alone publishes", [](auto& s) { s.submissionFps = 59.94; });
    metricOnly("nominal capture limit alone publishes", [](auto& s) { s.nominalSourceFps = 144; });
    metricOnly("capture count alone publishes", [](auto& s) { s.captureReceived = 50; });
    metricOnly("capture rate alone publishes", [](auto& s) { s.captureFps = 30; });
    metricOnly("scheduling wait alone publishes", [](auto& s) { s.schedulingWaitP95Ms = 3; });
    metricOnly("colour metadata alone publishes", [](auto& s) { s.colorStatus = L"SDR"; });
    metricOnly("HDR activity alone publishes", [](auto& s) { s.videoHdrActive = true; });
    metricOnly("settings metrics revision alone publishes", [](auto& s) { s.metrics.resolution.settingsRevision = 3; });
    auto timed = unchanged;
    timed.metrics.flow.gpuTiming[0] = {3.0, 4.0, 1};
    check(!ui::samePlayerSnapshot(timed, unchanged), "expired measurement publishes unknown instead of retaining old time");
    auto nan = unchanged; nan.metrics.flow.gpuTiming[0].mean = std::numeric_limits<double>::quiet_NaN();
    check(ui::samePlayerSnapshot(nan, nan), "stable unavailable NaN does not force continuous republishing");
    check(!ui::samePlayerSnapshot(nan, unchanged), "NaN availability differs from an absent sample");

    capture::BrowserTelemetrySource source;
    source.available = true; source.document = "extension/document-a";
    source.browser = {100, 20, 30}; source.media = {"video-a", 2, 1920, 1080, true};
    source.originalWidth = 1920; source.originalHeight = 1080; source.originalFps = 24;
    source.displayWidth = 3840; source.displayHeight = 2160; source.videoWidth = 2560; source.videoHeight = 1440;
    auto running = unchanged;
    running.sessionId = 7; running.running = true; running.capture = true;
    running.transport = engine::TransportState::Playing;
    running.desired.revision = running.applied.revision = 9;
    running.sourceWidth = 2560; running.sourceHeight = 1440; running.nominalSourceFps = 144;
    running.metrics.resolution = pipeline::ResolutionPlan::make({2560, 1440}, true,
        pipeline::NrSizePolicy::Realtime, false, 9);
    running.metrics.flow.latest.sessionId = 7;
    running.metrics.flow.latest.frame = {3, 9, 5};
    running.metrics.flow.lastReady100ns = 100;
    running.metrics.flow.lastPresent100ns = 120;
    running.metrics.flow.rateWindowReady = true;
    running.metrics.flow.gpuTiming[size_t(diagnostics::GpuStage::Sr)] = {2.5, 3.0, 8};
    running.submissionFps = 60; running.srActive = true;
    capture::BrowserTelemetryCache cache;
    cache.update(source, 7, true, running, 1000);
    check(cache.view().live && cache.view().last && cache.view().last->session == 7, "own live capture supplies a sample without a file source");
    check(cache.view().last->source.originalFps == 24 && running.nominalSourceFps == 144,
        "original video rate stays separate from compositor limit");
    check(cache.view().last->captureWidth == 2560 && cache.view().last->source.originalWidth == 1920,
        "original video and physical capture dimensions stay separate");
    check(cache.view().last->targetWidth == 3840 && cache.view().last->submitFps == 60,
        "target and measured submission rate come from the owned metrics window");
    cache.update(source, 7, true, running, 1200);
    check(cache.view().sampleAgeMs == 200, "unchanged present clock does not fabricate a newer sample");
    auto stopped = running; stopped.running = false; stopped.transport = engine::TransportState::Stopping;
    cache.update(source, 0, false, stopped, 1500);
    check(!cache.view().live && cache.view().last && cache.view().sampleAgeMs == 500,
        "paused or stopped capture retains explicitly stale measurements");
    check(capture::browserMonitorResumeAllowed(source, source, true, true),
        "paused binding permits fresh same-source playing report while Veyra monitor is foreground");
    auto longPause = cache;
    longPause.update(source, 0, false, stopped, 11000);
    check(!longPause.view().live && longPause.view().last && longPause.view().sampleAgeMs == 10000 &&
        capture::browserMonitorResumeAllowed(source, source, true, true),
        "ten-second pause retains identity for fresh resume without treating frozen data as active capture");
    check(!capture::browserMonitorResumeAllowed(source, source, false, true),
        "foreign foreground cannot resume a retained paused binding");
    check(!capture::browserMonitorResumeAllowed(source, source, true, false),
        "fully covered video cannot resume from paused authority");
    auto foreignVideo = source; foreignVideo.media.videoId = "video-b";
    check(!capture::browserMonitorResumeAllowed(source, foreignVideo, true, true),
        "different video cannot reuse paused own-monitor authority");
    auto legacy = source; legacy.media.hasIdentity = false;
    check(!capture::browserMonitorResumeAllowed(legacy, legacy, true, true),
        "legacy metadata without explicit video identity cannot authorize monitor continuation");
    check(!capture::browserMonitorResumeAllowed(source, legacy, true, true),
        "fresh metadata cannot drop the trusted video's identity on monitor resume");
    check(!capture::browserFinalForegroundAllowed(false, false),
        "external app taking foreground after region validation fails the final admission gate");
    check(capture::browserFinalForegroundAllowed(true, false) && capture::browserFinalForegroundAllowed(false, true),
        "final admission accepts only bound browser or known Veyra monitor foreground");
    check(capture::browserReportedFocusAllowed(false, true),
        "truthful unfocused report can continue only with already verified native monitor authority");
    check(!capture::browserReportedFocusAllowed(false, false),
        "unfocused Chrome first binding cannot establish monitor authority");
    check(!capture::browserReportedFocusAllowed(false, false) && !capture::browserFinalForegroundAllowed(false, false),
        "unfocused foreign foreground cannot admit browser capture");
    check(capture::browserReportedFocusAllowed(true, false) && !capture::browserFinalForegroundAllowed(false, false),
        "browser focus claim never overrides a foreign native foreground");
    check(!capture::browserReportedFocusAllowed(false,
        capture::browserMonitorResumeAllowed(source, foreignVideo, true, true)),
        "unfocused report for different video cannot reuse monitor authority");
    check(!capture::browserReportedFocusAllowed(false,
        capture::browserMonitorResumeAllowed(legacy, legacy, true, true)),
        "unfocused legacy report without explicit video identity remains forbidden");
    check(cache.view().last->snapshot && cache.view().last->snapshot->metrics.flow.gpuTiming[size_t(diagnostics::GpuStage::Sr)].mean == 2.5,
        "frozen GPU readout retains owned measured CPU values without retaining GPU resources");
    auto foreign = running; foreign.sessionId = 88; foreign.sourceWidth = 800;
    cache.update(source, 7, true, foreign, 1600);
    check(!cache.view().live && cache.view().last->session == 7 && cache.view().last->captureWidth == 2560,
        "foreign engine session cannot replace browser measurements");
    auto epoch = source; ++epoch.media.sourceEpoch;
    cache.update(epoch, 0, false, stopped, 1700);
    check(!cache.view().last, "new media epoch clears frozen measurements before opening");
    cache.update(source, 7, true, running, 1800);
    auto document = source; document.document = "extension/document-b";
    cache.update(document, 0, false, stopped, 1900);
    check(!cache.view().last, "new document cannot borrow previous video measurements");
    cache.update(source, 7, true, running, 2000);
    auto browser = source; ++browser.browser.processCreatedAt;
    cache.update(browser, 0, false, stopped, 2100);
    check(!cache.view().last, "reused browser process identity cannot borrow measurements");
    cache.update(source, 7, true, running, 2200);
    auto changing = running; changing.desired.revision = 10; changing.applying = true;
    cache.update(source, 7, true, changing, 2300);
    check(!cache.view().last && !cache.view().live, "new requested revision clears previous measured configuration");
    changing.applying = false;
    cache.update(source, 7, true, changing, 2350);
    check(!cache.view().last && !cache.view().live, "desired/applied mismatch cannot refill old sample during applying flag gap");
    cache.clearSample(); cache.update(source, 7, true, running, 2375, false);
    check(!cache.view().last && !cache.view().live, "queued controller configuration cannot refill old engine sample before requestSettings");
    auto unready = running; unready.metrics.flow.latest.frame.settingsRevision = 8;
    cache.update(source, 7, true, unready, 2400);
    check(!cache.view().last, "old timing window cannot become a new sample");
    auto unknown = running; unknown.submissionFps.reset();
    cache.update(source, 7, true, unknown, 2500);
    check(cache.view().last && !cache.view().last->submitFps, "unknown submit rate remains absent rather than measured zero");
    unknown.submissionFps = std::numeric_limits<double>::quiet_NaN();
    cache.update(source, 7, true, unknown, 2600);
    check(!cache.view().last->submitFps, "nonfinite submit rate remains unknown");
    auto warming = running; warming.metrics.flow.rateWindowReady = false;
    cache.update(source, 7, true, warming, 2700);
    check(!cache.view().last->submitFps, "rate warmup does not claim a measured zero");
    auto sdk = running; sdk.fgActive = true; sdk.applied.frameGenerationBackend = engine::FrameGenerationBackend::XeSS;
    sdk.metrics.flow.xessSdkSubmitFps = 120;
    cache.update(source, 7, true, sdk, 2800);
    check(cache.view().last->sdkSubmit && cache.view().last->submitFps == 120,
        "XeSS submission is labelled SDK and does not become physical display FPS");
    auto freshSession = running; freshSession.sessionId = 10; freshSession.metrics.flow.latest.sessionId = 10;
    freshSession.metrics.flow.lastReady100ns = 0;
    cache.update(source, 10, false, freshSession, 2900);
    check(!cache.view().last && !cache.view().live, "new capture session cannot show previous session as its first sample");
    auto unbound = source; unbound.browser = {};
    cache.clearSample(); cache.update(unbound, 7, true, running, 3000);
    check(!cache.view().last && !cache.view().live, "unbound metadata cannot authorize a browser sample");
    cache.clearSource();
    check(!cache.view().source.available && !cache.view().last, "new paused media can clear prior metadata and measurements together");
    std::cout << "browser_telemetry checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
