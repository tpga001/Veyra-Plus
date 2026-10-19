#include "veyra/capture/VideoRegionOverlay.h"
#include "veyra/NativePaintDiagnostics.h"
#include "veyra/Log.h"
#include "veyra/capture/BrowserAttachmentPolicy.h"
#include <dwmapi.h>

#include <cstdint>
#include <algorithm>
#include <format>

namespace veyra::capture {
namespace {
constexpr wchar_t kOverlayClass[] = L"VeyraVideoRegionOverlay";
constexpr DWORD kExcludeFromCapture = 0x00000011; // WDA_EXCLUDEFROMCAPTURE

HWND rootWindow(HWND window) {
    return window ? GetAncestor(window, GA_ROOT) : nullptr;
}
policy::Bounds bounds(const RECT& rectangle) {
    return {double(rectangle.left),double(rectangle.top),double(rectangle.right),double(rectangle.bottom)};
}
BOOL CALLBACK collectDisplay(HMONITOR, HDC, LPRECT rectangle, LPARAM parameter) {
    auto& displays = *reinterpret_cast<std::vector<policy::Bounds>*>(parameter);
    displays.push_back(bounds(*rectangle));
    return displays.size() <= 32;
}
} // namespace

void VideoRegionOverlay::setMonitorWindow(HWND monitorWindow) {
    if (!onOwnerThread("setMonitorWindow-thread")) return;
    const HWND root = rootWindow(monitorWindow);
    DWORD process = 0;
    if (root) GetWindowThreadProcessId(root, &process);
    monitorWindow_ = root && process == GetCurrentProcessId() ? root : nullptr;
    if (window_ && hasGeometry_ && !sourceForeground()) hide();
}

bool VideoRegionOverlay::knownMonitorForeground(HWND monitor, HWND ignoredOverlay) {
    const HWND foreground = rootWindow(GetForegroundWindow());
    DWORD monitorProcess = 0, foregroundProcess = 0;
    if (monitor) GetWindowThreadProcessId(monitor, &monitorProcess);
    if (foreground) GetWindowThreadProcessId(foreground, &foregroundProcess);
    // A valid owned modal disables its main owner. Require the actual foreground
    // window to be usable while retaining the exact registered root-owner gate.
    return foreground && monitor && IsWindow(foreground) && IsWindow(monitor) &&
        (foreground == monitor || GetAncestor(foreground, GA_ROOTOWNER) == monitor) && foreground != ignoredOverlay &&
        monitorProcess == GetCurrentProcessId() && foregroundProcess == monitorProcess &&
        IsWindowVisible(foreground) && !IsIconic(foreground) && IsWindowEnabled(foreground) &&
        IsWindowVisible(monitor) && !IsIconic(monitor);
}

bool VideoRegionOverlay::foregroundRegionAllowed(HWND browser, HWND monitor, HWND ignoredOverlay,
                                                 const RECT& videoRegionScreen) {
    if (!browser || !IsWindow(browser) || !IsWindowVisible(browser) || IsIconic(browser) ||
        !IsWindowEnabled(browser)) return false;
    const HWND foreground = rootWindow(GetForegroundWindow());
    const bool ownMonitor = knownMonitorForeground(monitor, ignoredOverlay);
    if (foreground != browser && !ownMonitor) return false;
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(browser, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked)
        return false;
    std::vector<policy::Bounds> displays, occluders;
    if (!EnumDisplayMonitors(nullptr, nullptr, collectDisplay, reinterpret_cast<LPARAM>(&displays))) return false;
    unsigned traversed = 0;
    for (HWND above = GetWindow(browser, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
        if (++traversed > 256) return false;
        if (above == ignoredOverlay || !IsWindowVisible(above) || IsIconic(above)) continue;
        cloaked = 0;
        if (FAILED(DwmGetWindowAttribute(above, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) return false;
        if (cloaked) continue;
        RECT rectangle{};
        if (FAILED(DwmGetWindowAttribute(above, DWMWA_EXTENDED_FRAME_BOUNDS, &rectangle, sizeof(rectangle))))
            return false;
        if (rectangle.right > rectangle.left && rectangle.bottom > rectangle.top)
            occluders.push_back(bounds(rectangle));
    }
    return policy::foregroundAuthority({foreground == browser,ownMonitor,true,true,false,
        policy::anyRegionVisible(bounds(videoRegionScreen),displays,occluders)}) != policy::ForegroundAuthority::Reject;
}

VideoRegionOverlay::~VideoRegionOverlay() {
    destroy();
}

bool VideoRegionOverlay::fail(const char* operation, DWORD error) {
    lastError_ = error ? error : ERROR_GEN_FAILURE;
    log::error("video-overlay", std::format(
        "operation={} error={} window={} source={}",
        operation, lastError_, uint64_t(window_), uint64_t(source_)));
    return false;
}

bool VideoRegionOverlay::onOwnerThread(const char* operation) {
    if (!ownerThreadId_ || ownerThreadId_ == GetCurrentThreadId()) return true;
    return fail(operation, ERROR_INVALID_THREAD_ID);
}

bool VideoRegionOverlay::validGeometry(const RECT& rectangle) const {
    const int64_t width = int64_t(rectangle.right) - rectangle.left;
    const int64_t height = int64_t(rectangle.bottom) - rectangle.top;
    return width > 0 && height > 0 && width <= 16384 && height <= 16384;
}

bool VideoRegionOverlay::create(HWND sourceWindow, const RECT& rectangle,
                                HINSTANCE instance) {
    if (window_) return fail("create-already-open", ERROR_ALREADY_EXISTS);
    if (!IsWindow(sourceWindow) || !validGeometry(rectangle))
        return fail("create-invalid-source-or-rectangle", ERROR_INVALID_PARAMETER);
    ownerThreadId_ = GetCurrentThreadId();
    lastError_ = ERROR_SUCCESS;
    if (!instance) instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kOverlayClass;
    const ATOM atom = RegisterClassExW(&windowClass);
    if (!atom) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) return fail("RegisterClassExW", error);
        WNDCLASSEXW existing{sizeof(existing)};
        if (!GetClassInfoExW(instance, kOverlayClass, &existing) ||
            existing.lpfnWndProc != windowProc)
            return fail("GetClassInfoExW", ERROR_INVALID_DATA);
    }

    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        kOverlayClass, L"Veyra Video Enhancement",
        WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        rectangle.left, rectangle.top,
        int(int64_t(rectangle.right) - rectangle.left),
        int(int64_t(rectangle.bottom) - rectangle.top),
        nullptr, nullptr, instance, this);
    if (!window_) return fail("CreateWindowExW", GetLastError());

    const auto abortCreate = [this](const char* operation, DWORD error) {
        fail(operation, error);
        destroy();
        return false;
    };
    if (!SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA))
        return abortCreate("SetLayeredWindowAttributes", GetLastError());
    if (!SetWindowDisplayAffinity(window_, kExcludeFromCapture))
        return abortCreate("SetWindowDisplayAffinity", GetLastError());
    DWORD affinity = 0;
    if (!GetWindowDisplayAffinity(window_, &affinity))
        return abortCreate("GetWindowDisplayAffinity", GetLastError());
    if (affinity != kExcludeFromCapture)
        return abortCreate("capture-exclusion-verification", ERROR_INVALID_DATA);

    if (!updateGeometry(sourceWindow, rectangle)) {
        destroy();
        return false;
    }
    log::info("video-overlay", std::format(
        "created hidden window={} source={} rect={},{},{},{} affinity=0x{:X} topmost=0",
        uint64_t(window_), uint64_t(source_), rectangle.left, rectangle.top,
        rectangle.right, rectangle.bottom, affinity));
    return true;
}

bool VideoRegionOverlay::updateGeometry(HWND sourceWindow, const RECT& rectangle) {
    if (!onOwnerThread("updateGeometry-thread")) return false;
    if (!window_ || !IsWindow(window_) || !IsWindow(sourceWindow) ||
        !validGeometry(rectangle))
        return fail("updateGeometry-invalid-source-or-rectangle", ERROR_INVALID_PARAMETER);
    const HWND sourceRoot = rootWindow(sourceWindow);
    if (!sourceRoot || sourceRoot == window_)
        return fail("updateGeometry-invalid-root", ERROR_INVALID_WINDOW_HANDLE);
    DWORD processId = 0;
    GetWindowThreadProcessId(sourceRoot, &processId);
    if (!processId) return fail("GetWindowThreadProcessId", GetLastError());
    const HMONITOR monitor = MonitorFromRect(&rectangle, MONITOR_DEFAULTTONEAREST);
    const bool newSource = source_ != sourceRoot || sourceProcessId_ != processId;
    const bool newSize = !hasGeometry_ ||
        int64_t(geometry_.right) - geometry_.left != int64_t(rectangle.right) - rectangle.left ||
        int64_t(geometry_.bottom) - geometry_.top != int64_t(rectangle.bottom) - rectangle.top;
    if (newSource || newSize || monitor_ != monitor) hide();
    const bool changed = !hasGeometry_ || newSource ||
        geometry_.left != rectangle.left || geometry_.top != rectangle.top ||
        geometry_.right != rectangle.right || geometry_.bottom != rectangle.bottom;
    if (changed) {
        if (!SetWindowPos(window_, nullptr, rectangle.left, rectangle.top,
                          int(int64_t(rectangle.right) - rectangle.left),
                          int(int64_t(rectangle.bottom) - rectangle.top),
                          SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER))
            return fail("SetWindowPos-geometry", GetLastError());
    }
    source_ = sourceRoot;
    sourceProcessId_ = processId;
    if (newSource) warnedTopmost_ = false;
    monitor_ = monitor;
    geometry_ = rectangle;
    hasGeometry_ = true;
    lastError_ = ERROR_SUCCESS;
    if (changed)
        log::info("video-overlay", std::format(
            "geometry source={} rect={},{},{},{} resized={} monitor={}",
            uint64_t(source_), rectangle.left, rectangle.top,
            rectangle.right, rectangle.bottom, newSize, uint64_t(monitor)));
    if (!sourceForeground()) hide();
    return true;
}

bool VideoRegionOverlay::sourceForeground() const {
    if (!source_ || !IsWindow(source_) || !IsWindowVisible(source_) || IsIconic(source_))
        return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(source_, &processId);
    return processId == sourceProcessId_ &&
        foregroundRegionAllowed(source_, monitorWindow_, window_, geometry_);
}

bool VideoRegionOverlay::setExcludedRects(const std::vector<RECT>& rectangles) {
    if (!onOwnerThread("setExcludedRects-thread")) return false;
    if (!window_ || !hasGeometry_ || rectangles.size() > 96)
        return fail("setExcludedRects-invalid-state", ERROR_INVALID_PARAMETER);
    const LONG width = geometry_.right - geometry_.left;
    const LONG height = geometry_.bottom - geometry_.top;
    std::vector<RECT> clipped;
    clipped.reserve(rectangles.size());
    const RECT bounds{0, 0, width, height};
    for (const auto& rectangle : rectangles) {
        RECT intersection{};
        if (IntersectRect(&intersection, &rectangle, &bounds)) clipped.push_back(intersection);
    }
    const bool same = regionWidth_ == width && regionHeight_ == height &&
        clipped.size() == excludedRects_.size() &&
        std::equal(clipped.begin(), clipped.end(), excludedRects_.begin(),
            [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; });
    if (same) return true;
    HRGN region = CreateRectRgn(0, 0, width, height);
    if (!region) return fail("CreateRectRgn", GetLastError());
    for (const auto& rectangle : clipped) {
        HRGN hole = CreateRectRgn(rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
        if (!hole) { DeleteObject(region); return fail("CreateRectRgn-hole", GetLastError()); }
        const int combined = CombineRgn(region, region, hole, RGN_DIFF);
        DeleteObject(hole);
        if (combined == ERROR) { DeleteObject(region); return fail("CombineRgn", GetLastError()); }
    }
    // A successful SetWindowRgn transfers ownership of region to Windows.
    if (!SetWindowRgn(window_, region, TRUE)) {
        const DWORD error = GetLastError(); DeleteObject(region);
        return fail("SetWindowRgn", error);
    }
    excludedRects_ = std::move(clipped);
    regionWidth_ = width; regionHeight_ = height;
    lastError_ = ERROR_SUCCESS;
    return true;
}

bool VideoRegionOverlay::showAfterPresent() {
    if (!onOwnerThread("showAfterPresent-thread")) return false;
    if (!window_ || !IsWindow(window_) || !hasGeometry_)
        return fail("showAfterPresent-not-created", ERROR_INVALID_WINDOW_HANDLE);
    if (!sourceForeground()) {
        hide();
        lastError_ = ERROR_NOT_READY;
        return false;
    }
    // A non-topmost surface cannot safely cover a topmost source. Never promote
    // this window as a workaround: leave the original player visible instead.
    if (GetWindowLongPtrW(source_, GWL_EXSTYLE) & WS_EX_TOPMOST) {
        hide();
        lastError_ = ERROR_NOT_SUPPORTED;
        if (!warnedTopmost_) {
            log::warn("video-overlay", "source is topmost; overlay stays hidden to preserve non-topmost policy");
            warnedTopmost_ = true;
        }
        return false;
    }
    warnedTopmost_ = false;
    if (GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) {
        if (!SetWindowPos(window_, HWND_NOTOPMOST, 0, 0, 0, 0,
                          SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER))
            return fail("SetWindowPos-demote", GetLastError());
        log::warn("video-overlay", "unexpected topmost style removed from this overlay");
    }
    const bool wasVisible = visible();
    // Insert directly above the browser, below every intervening ordinary
    // window. HWND_TOP would cover the monitor UI or unrelated app windows.
    const HWND predecessor = GetWindow(source_, GW_HWNDPREV);
    const bool browserForeground = rootWindow(GetForegroundWindow()) == source_;
    if (!wasVisible || predecessor != window_) {
        const HWND previous = predecessor == window_ ? GetWindow(window_, GW_HWNDPREV) : predecessor;
        const auto placement = policy::overlayPlacement(browserForeground, previous != nullptr,
            previous && (GetWindowLongPtrW(previous, GWL_EXSTYLE) & WS_EX_TOPMOST));
        if (placement == policy::OverlayPlacement::Reject) { hide(); lastError_ = ERROR_NOT_READY; return false; }
        const HWND insertAfter = placement == policy::OverlayPlacement::AboveSource ? previous : HWND_TOP;
        if (!SetWindowPos(window_, insertAfter, 0, 0, 0, 0,
                          SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE |
                              SWP_NOOWNERZORDER | SWP_SHOWWINDOW))
            return fail("SetWindowPos-show", GetLastError());
    }
    if (!sourceForeground()) {
        hide(); // The user may have changed applications between these checks.
        lastError_ = ERROR_NOT_READY;
        return false;
    }
    if (GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) {
        hide();
        if (!SetWindowPos(window_, HWND_NOTOPMOST, 0, 0, 0, 0,
                          SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER))
            return fail("SetWindowPos-demote-after-show", GetLastError());
        return fail("show-unexpected-topmost", ERROR_INVALID_STATE);
    }
    if (GetWindow(source_, GW_HWNDPREV) != window_) {
        hide(); return fail("show-source-z-order-changed", ERROR_NOT_READY);
    }
    if (!visible()) return fail("show-not-visible", ERROR_INVALID_STATE);
    lastError_ = ERROR_SUCCESS;
    if (!wasVisible)
        log::info("video-overlay", std::format(
            "visible window={} source={} foreground={} topmost=0",
            uint64_t(window_), uint64_t(source_), uint64_t(GetForegroundWindow())));
    return true;
}

void VideoRegionOverlay::hide() {
    if (!onOwnerThread("hide-thread")) return;
    if (window_ && IsWindow(window_) && IsWindowVisible(window_)) {
        ShowWindow(window_, SW_HIDE);
        log::info("video-overlay", std::format("hidden window={} source={}",
                                             uint64_t(window_), uint64_t(source_)));
    }
}

bool VideoRegionOverlay::visible() const noexcept {
    return window_ && IsWindow(window_) && IsWindowVisible(window_);
}

void VideoRegionOverlay::destroy() noexcept {
    if (!window_) return;
    const HWND oldWindow = window_;
    // Detach first so even a failed or deferred destruction cannot retain a
    // dangling C++ pointer in the window procedure.
    SetWindowLongPtrW(oldWindow, GWLP_USERDATA, 0);
    if (ownerThreadId_ == GetCurrentThreadId()) {
        if (!DestroyWindow(oldWindow)) {
            const DWORD error = GetLastError();
            lastError_ = error;
            try { log::error("video-overlay", std::format("DestroyWindow error={}", error)); }
            catch (...) {}
        }
    } else {
        const BOOL posted = PostMessageW(oldWindow, WM_CLOSE, 0, 0);
        lastError_ = posted ? ERROR_INVALID_THREAD_ID : GetLastError();
        try { log::warn("video-overlay", std::format(
            "destroy called on wrong thread; detached window close posted={} error={}",
            posted != FALSE, lastError_)); }
        catch (...) {}
    }
    window_ = nullptr;
    source_ = nullptr;
    sourceProcessId_ = ownerThreadId_ = 0;
    hasGeometry_ = false;
    warnedTopmost_ = false;
    monitor_ = nullptr;
    geometry_ = {};
    excludedRects_.clear();
    regionWidth_ = regionHeight_ = 0;
}

LRESULT CALLBACK VideoRegionOverlay::windowProc(HWND window, UINT message,
                                               WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR previous = SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        if (!previous && GetLastError() != ERROR_SUCCESS) return FALSE;
        return TRUE;
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        diagnostics::NativePaintScope paintScope(
            diagnostics::overlayPaint, reinterpret_cast<std::uintptr_t>(window),
            static_cast<std::uint32_t>(GetCurrentThreadId()));
        // Complete the native paint cycle even though DXGI supplies the pixels.
        // A ValidateRect-only handler allowed repeated overlay WM_PAINT dispatch
        // to starve Qt timers while the enhancement graph was initializing.
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        auto* self = reinterpret_cast<VideoRegionOverlay*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self && self->window_ == window) self->window_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace veyra::capture
