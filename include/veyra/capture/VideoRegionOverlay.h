#pragma once

#include <windows.h>
#include <vector>

namespace veyra::capture {

// A presentation HWND only. The caller owns capture, GPU presentation and the
// foreground policy. All window operations belong to the creating UI thread.
// Screen rectangles are physical pixels and may have negative monitor origins.
class VideoRegionOverlay final {
public:
    VideoRegionOverlay() = default;
    ~VideoRegionOverlay();

    VideoRegionOverlay(const VideoRegionOverlay&) = delete;
    VideoRegionOverlay& operator=(const VideoRegionOverlay&) = delete;

    // Creates a hidden, capture-excluded window. A failed exclusion is fatal:
    // the enhanced surface must never feed back into its own capture source.
    bool create(HWND sourceWindow, const RECT& videoRegionScreen,
                HINSTANCE instance = nullptr);
    bool updateGeometry(HWND sourceWindow, const RECT& videoRegionScreen);
    // Explicit main monitor root only; registration survives capture recreation.
    void setMonitorWindow(HWND monitorWindow);
    // Same known main-window family gate for controller and overlay checks.
    static bool knownMonitorForeground(HWND monitor, HWND ignoredOverlay = nullptr);
    // Native authority and visible-screen proof shared with the controller.
    static bool foregroundRegionAllowed(HWND browser, HWND monitor, HWND ignoredOverlay,
                                        const RECT& videoRegionScreen);
    // Local physical-pixel holes leave webpage controls/subtitles visible.
    // Their pixels come from the browser underneath, never from a CPU copy.
    bool setExcludedRects(const std::vector<RECT>& localRectangles);

    // Call only after a frame matching the current source/extent has presented.
    // This defensively checks the foreground again and never activates a window
    // or makes this HWND globally topmost.
    bool showAfterPresent();
    void hide();
    void destroy() noexcept;

    HWND handle() const noexcept { return window_; }
    HWND sourceWindow() const noexcept { return source_; }
    const RECT& geometry() const noexcept { return geometry_; }
    bool visible() const noexcept;
    DWORD lastError() const noexcept { return lastError_; }

private:
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    bool fail(const char* operation, DWORD error);
    bool onOwnerThread(const char* operation);
    bool validGeometry(const RECT&) const;
    bool sourceForeground() const;

    HWND window_ = nullptr;
    HWND source_ = nullptr;
    HWND monitorWindow_ = nullptr;
    DWORD sourceProcessId_ = 0;
    DWORD ownerThreadId_ = 0;
    DWORD lastError_ = ERROR_SUCCESS;
    HMONITOR monitor_ = nullptr;
    RECT geometry_{};
    bool hasGeometry_ = false;
    bool warnedTopmost_ = false;
    std::vector<RECT> excludedRects_;
    LONG regionWidth_ = 0, regionHeight_ = 0;
};

} // namespace veyra::capture
