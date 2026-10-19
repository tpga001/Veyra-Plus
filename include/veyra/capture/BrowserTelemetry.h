#pragma once
#include "veyra/capture/BrowserAttachmentPolicy.h"
#include "veyra/engine/EngineController.h"
#include <cmath>
#include <optional>
#include <string>

namespace veyra::capture {
// In-process identity and metadata. HTTP diagnostics are a serialization of
// this source, never the authority used to attach engine measurements.
struct BrowserTelemetrySource {
    bool available = false;
    std::string document;
    policy::BrowserIdentity browser;
    policy::MediaFingerprint media;
    unsigned originalWidth = 0, originalHeight = 0;
    double originalFps = 0;
    unsigned displayWidth = 0, displayHeight = 0, videoWidth = 0, videoHeight = 0;
};
inline bool sameTelemetrySource(const BrowserTelemetrySource& a, const BrowserTelemetrySource& b) {
    return a.available && b.available && a.document == b.document &&
        policy::sameBrowser(a.browser, b.browser) && policy::sameMedia(a.media, b.media);
}
inline bool browserMonitorResumeAllowed(const BrowserTelemetrySource& trusted, const BrowserTelemetrySource& fresh,
                                       bool ownMonitorForeground, bool regionVisible) {
    return ownMonitorForeground && regionVisible && trusted.media.hasIdentity && fresh.media.hasIdentity &&
        sameTelemetrySource(trusted, fresh);
}
inline bool browserFinalForegroundAllowed(bool browserForeground, bool ownMonitorForeground) {
    return browserForeground || ownMonitorForeground;
}
inline bool browserReportedFocusAllowed(bool reportedFocused, bool trustedOwnMonitorForeground) {
    // An unfocused browser is eligible only after the native foreground,
    // bound video identity and current visible region have all been verified.
    return reportedFocused || trustedOwnMonitorForeground;
}
struct BrowserTelemetrySample {
    // Immutable CPU values from the owned session. No frame surfaces, queues
    // or GPU objects are retained by the monitor's frozen readouts.
    std::shared_ptr<const engine::PlayerSnapshot> snapshot;
    BrowserTelemetrySource source;
    uint64_t session = 0, epoch = 0, revision = 0, sampledAtMs = 0;
    int64_t presentedAt100ns = 0;
    unsigned captureWidth = 0, captureHeight = 0, targetWidth = 0, targetHeight = 0;
    bool nrActive = false, srActive = false, fgActive = false;
    std::optional<double> submitFps;
    bool sdkSubmit = false;
};
struct BrowserTelemetryView {
    BrowserTelemetrySource source;
    std::optional<BrowserTelemetrySample> last;
    bool live = false;
    uint64_t sampleAgeMs = 0;
};
class BrowserTelemetryCache {
    BrowserTelemetryView view_;
public:
    // Called on the UI thread at the controller's publication cadence. Only
    // its own engine session and the applied metrics window can supply samples.
    void update(const BrowserTelemetrySource& source, uint64_t ownedSession,
                bool overlayVisible, const engine::PlayerSnapshot& s, uint64_t nowMs,
                bool configurationReady = true) {
        if (source.available) {
            if (view_.source.available && !sameTelemetrySource(view_.source, source)) view_.last.reset();
            view_.source = source;
        }
        const bool own = ownedSession && s.sessionId == ownedSession;
        if (own && view_.last && (view_.last->session != ownedSession ||
            view_.last->revision != s.desired.revision)) view_.last.reset();
        const auto& flow = s.metrics.flow;
        const auto& plan = s.metrics.resolution;
        const bool ready = configurationReady && source.available && source.browser.valid() && own && s.running && s.capture && !s.applying &&
            s.desired.revision == s.applied.revision &&
            s.transport == engine::TransportState::Playing && flow.latest.sessionId == ownedSession &&
            flow.latest.frame.settingsRevision == s.applied.revision && flow.lastReady100ns > 0 && flow.lastPresent100ns > 0 &&
            plan.settingsRevision == s.applied.revision && plan.output.valid();
        if (ready) {
            BrowserTelemetrySample sample;
            sample.snapshot = std::make_shared<const engine::PlayerSnapshot>(s);
            sample.source = source; sample.session = ownedSession;
            sample.epoch = flow.latest.frame.epoch; sample.revision = s.applied.revision;
            sample.presentedAt100ns = flow.lastPresent100ns;
            sample.sampledAtMs = view_.last && view_.last->session == sample.session &&
                view_.last->epoch == sample.epoch && view_.last->revision == sample.revision &&
                view_.last->presentedAt100ns == sample.presentedAt100ns ? view_.last->sampledAtMs : nowMs;
            sample.captureWidth = s.sourceWidth; sample.captureHeight = s.sourceHeight;
            sample.targetWidth = plan.output.width; sample.targetHeight = plan.output.height;
            sample.nrActive = s.nrActive; sample.srActive = s.srActive; sample.fgActive = s.fgActive;
            sample.sdkSubmit = s.fgActive && s.applied.frameGenerationBackend == engine::FrameGenerationBackend::XeSS;
            if (flow.rateWindowReady) {
                const auto rate = sample.sdkSubmit ? std::optional<double>(flow.xessSdkSubmitFps) : s.submissionFps;
                if (rate && std::isfinite(*rate) && *rate >= 0) sample.submitFps = rate;
            }
            view_.last = std::move(sample);
        }
        view_.live = ready && overlayVisible && view_.last.has_value();
        view_.sampleAgeMs = view_.last && nowMs >= view_.last->sampledAtMs ? nowMs - view_.last->sampledAtMs : 0;
    }
    const BrowserTelemetryView& view() const { return view_; }
    void clearSample() { view_.last.reset(); view_.live = false; view_.sampleAgeMs = 0; }
    void clearSource() { view_ = {}; }
};
} // namespace veyra::capture
