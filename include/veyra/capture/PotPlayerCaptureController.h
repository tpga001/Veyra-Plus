#pragma once
#include "veyra/capture/PotPlayerVideoProbe.h"
#include "veyra/capture/VideoRegionOverlay.h"
#include "veyra/engine/EngineController.h"
#include <string>

namespace veyra::capture {
// All methods run on the UI thread. The existing engine owns GPU work.
class PotPlayerCaptureController final {
public:
    PotPlayerCaptureController();
    ~PotPlayerCaptureController();
    void setEnabled(bool enabled);
    bool enabled() const noexcept { return enabled_; }
    void tick();
    std::wstring status() const { return status_; }
    engine::PlayerSnapshot snapshot() const { return engine_.snapshot(); }
    HWND outputWindow() const { return overlay_.handle(); }
private:
    void stopSession(const wchar_t* reason);
    void fail(const wchar_t* reason);
    bool start(const PotPlayerVideoGeometry&);
    // Engine is destroyed first so its GPU worker releases the HWND before the overlay.
    VideoRegionOverlay overlay_;
    engine::EngineController engine_;
    PotPlayerVideoGeometry active_{}, candidate_{};
    bool enabled_=false, haveCandidate_=false, opening_=false;
    unsigned cropMismatches_=0;
    ULONGLONG stableSince_=0, openedAt_=0, nextLog_=0;
    std::wstring status_=L"已关闭";
};
}
