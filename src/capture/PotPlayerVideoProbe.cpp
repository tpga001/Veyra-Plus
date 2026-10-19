#include "veyra/capture/PotPlayerVideoProbe.h"

#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace veyra::capture {
namespace {

class ScopedPhysicalCoordinates {
public:
    ScopedPhysicalCoordinates() noexcept {
        previous_ = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~ScopedPhysicalCoordinates() {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }
    bool valid() const noexcept { return previous_ != nullptr; }
private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

class ProcessHandle {
public:
    explicit ProcessHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ProcessHandle() { if (handle_) CloseHandle(handle_); }
    HANDLE get() const noexcept { return handle_; }
private:
    HANDLE handle_ = nullptr;
};

bool validRect(const RECT& rect) noexcept {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool ordinalEquals(std::wstring_view a, std::wstring_view b) noexcept {
    return a.size() == b.size() &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
            b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool supportedClass(HWND window) noexcept {
    std::array<wchar_t, 64> name{};
    const int length = GetClassNameW(window, name.data(), static_cast<int>(name.size()));
    return length > 0 && std::wstring_view(name.data(), static_cast<size_t>(length)) == L"PotPlayer64";
}

bool supportedProcess(DWORD processId, DWORD& error, bool& queried) noexcept {
    queried = false;
    ProcessHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!process.get()) {
        error = GetLastError();
        return false;
    }
    std::array<wchar_t, 32768> path{};
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)) {
        error = GetLastError();
        return false;
    }
    queried = true;
    const std::wstring_view fullPath(path.data(), length);
    const auto slash = fullPath.find_last_of(L"\\/");
    const auto baseName = slash == std::wstring_view::npos ? fullPath : fullPath.substr(slash + 1);
    return ordinalEquals(baseName, L"PotPlayerMini64.exe");
}

struct CanvasSearch {
    HWND player = nullptr;
    DWORD processId = 0;
    bool failed = false;
    std::vector<PotPlayerCanvasCandidate> candidates;
};

BOOL CALLBACK collectCanvas(HWND child, LPARAM parameter) noexcept {
    auto& search = *reinterpret_cast<CanvasSearch*>(parameter);
    if (!IsWindowVisible(child) || GetAncestor(child, GA_ROOT) != search.player) return TRUE;
    DWORD processId = 0;
    if (!GetWindowThreadProcessId(child, &processId) || processId != search.processId) return TRUE;
    std::array<wchar_t, 64> className{};
    const int length = GetClassNameW(child, className.data(), static_cast<int>(className.size()));
    if (length <= 0 ||
        std::wstring_view(className.data(), static_cast<size_t>(length)) != L"AfxWnd140su") return TRUE;
    RECT rect{};
    if (!GetWindowRect(child, &rect)) return TRUE;
    try {
        search.candidates.push_back({child, rect, true, true});
    } catch (...) {
        search.failed = true;
        return FALSE;
    }
    return TRUE;
}

} // namespace

std::optional<PotPlayerCanvasCandidate> selectPotPlayerVideoCanvas(
    std::span<const PotPlayerCanvasCandidate> candidates,
    const RECT& playerClientRect) noexcept {
    if (!validRect(playerClientRect)) return std::nullopt;
    std::optional<PotPlayerCanvasCandidate> best;
    uint64_t bestArea = 0;
    for (const auto& candidate : candidates) {
        if (!candidate.window || !candidate.visible || !candidate.matchesVideoClass ||
            !validRect(candidate.physicalScreenRect)) continue;
        RECT clipped{
            (std::max)(candidate.physicalScreenRect.left, playerClientRect.left),
            (std::max)(candidate.physicalScreenRect.top, playerClientRect.top),
            (std::min)(candidate.physicalScreenRect.right, playerClientRect.right),
            (std::min)(candidate.physicalScreenRect.bottom, playerClientRect.bottom)
        };
        const int64_t width = int64_t(clipped.right) - clipped.left;
        const int64_t height = int64_t(clipped.bottom) - clipped.top;
        if (width < 64 || height < 64) continue;
        const uint64_t area = uint64_t(width) * uint64_t(height);
        if (area > bestArea) {
            best = candidate;
            best->physicalScreenRect = clipped;
            bestArea = area;
        }
    }
    return best;
}

PotPlayerVideoProbeResult probePotPlayerVideoWindow(
    HWND window, DWORD expectedProcessId) noexcept {
    PotPlayerVideoProbeResult result;
    auto reject = [&](PotPlayerProbeStatus status, DWORD error = ERROR_SUCCESS) {
        result.status = status;
        result.win32Error = error;
        return result;
    };
    if (!window) return reject(PotPlayerProbeStatus::NoWindow);
    result.geometry.playerWindow = window;
    if (!IsWindow(window)) return reject(PotPlayerProbeStatus::InvalidWindow);
    if (GetAncestor(window, GA_ROOT) != window) return reject(PotPlayerProbeStatus::NotTopLevel);
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(window, &processId);
    result.geometry.processId = processId;
    if (!threadId || !processId)
        return reject(PotPlayerProbeStatus::InvalidWindow, GetLastError());
    if (expectedProcessId && processId != expectedProcessId)
        return reject(PotPlayerProbeStatus::ProcessMismatch);
    if (processId == GetCurrentProcessId()) return reject(PotPlayerProbeStatus::OwnProcess);
    if (!supportedClass(window)) return reject(PotPlayerProbeStatus::UnsupportedIdentity);
    bool identityQueried = false;
    DWORD error = ERROR_SUCCESS;
    if (!supportedProcess(processId, error, identityQueried))
        return reject(identityQueried ? PotPlayerProbeStatus::UnsupportedIdentity :
            PotPlayerProbeStatus::IdentityUnavailable, error);
    if (!IsWindowVisible(window)) return reject(PotPlayerProbeStatus::Hidden);
    if (IsIconic(window)) return reject(PotPlayerProbeStatus::Minimized);
    DWORD cloaked = 0;
    const HRESULT cloakResult = DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (FAILED(cloakResult)) return reject(PotPlayerProbeStatus::GeometryUnavailable);
    if (cloaked) return reject(PotPlayerProbeStatus::Cloaked);
    if (!IsWindowEnabled(window)) return reject(PotPlayerProbeStatus::Disabled);
    if (GetAncestor(GetForegroundWindow(), GA_ROOT) != window)
        return reject(PotPlayerProbeStatus::NotForeground);
    GUITHREADINFO gui{sizeof(GUITHREADINFO)};
    if (!GetGUIThreadInfo(threadId, &gui))
        return reject(PotPlayerProbeStatus::InputStateUnavailable, GetLastError());
    if (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))
        return reject(PotPlayerProbeStatus::MenuActive);
    if (gui.flags & GUI_INMOVESIZE) return reject(PotPlayerProbeStatus::MoveSizeActive);

    ScopedPhysicalCoordinates physical;
    if (!physical.valid()) return reject(PotPlayerProbeStatus::DpiContextUnavailable, GetLastError());
    auto& geometry = result.geometry;
    geometry.dpi = GetDpiForWindow(window);
    if (!geometry.dpi || !GetWindowRect(window, &geometry.windowRect) ||
        !GetClientRect(window, &geometry.clientRect))
        return reject(PotPlayerProbeStatus::GeometryUnavailable, GetLastError());
    POINT origin{geometry.clientRect.left, geometry.clientRect.top};
    if (!ClientToScreen(window, &origin))
        return reject(PotPlayerProbeStatus::GeometryUnavailable, GetLastError());
    const LONG clientWidth = geometry.clientRect.right - geometry.clientRect.left;
    const LONG clientHeight = geometry.clientRect.bottom - geometry.clientRect.top;
    geometry.clientRect = {origin.x, origin.y, origin.x + clientWidth, origin.y + clientHeight};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
        &geometry.extendedFrameRect, sizeof(geometry.extendedFrameRect))) ||
        !validRect(geometry.windowRect) || !validRect(geometry.clientRect) ||
        !validRect(geometry.extendedFrameRect))
        return reject(PotPlayerProbeStatus::GeometryUnavailable);

    CanvasSearch search{window, processId};
    EnumChildWindows(window, collectCanvas, reinterpret_cast<LPARAM>(&search));
    if (search.failed) return reject(PotPlayerProbeStatus::GeometryUnavailable, ERROR_NOT_ENOUGH_MEMORY);
    const auto canvas = selectPotPlayerVideoCanvas(search.candidates, geometry.clientRect);
    if (!canvas) return reject(PotPlayerProbeStatus::VideoCanvasNotFound);
    geometry.videoWindow = canvas->window;
    geometry.videoRect = canvas->physicalScreenRect;
    geometry.monitor = MonitorFromRect(&geometry.videoRect, MONITOR_DEFAULTTONEAREST);
    if (!geometry.monitor) return reject(PotPlayerProbeStatus::GeometryUnavailable, GetLastError());

    // Enumeration/geometry are separate queries. Revalidate the handle and
    // authority before allowing the caller to start a capture session.
    DWORD finalProcessId = 0, finalVideoProcessId = 0;
    if (!IsWindow(window) || !IsWindow(geometry.videoWindow) ||
        !GetWindowThreadProcessId(window, &finalProcessId) || finalProcessId != processId ||
        !GetWindowThreadProcessId(geometry.videoWindow, &finalVideoProcessId) ||
        finalVideoProcessId != processId ||
        GetAncestor(geometry.videoWindow, GA_ROOT) != window ||
        !IsWindowVisible(window) || IsIconic(window) || !IsWindowVisible(geometry.videoWindow))
        return reject(PotPlayerProbeStatus::WindowChanged);
    if (GetAncestor(GetForegroundWindow(), GA_ROOT) != window)
        return reject(PotPlayerProbeStatus::NotForeground);
    GUITHREADINFO finalGui{sizeof(GUITHREADINFO)};
    if (!GetGUIThreadInfo(threadId, &finalGui))
        return reject(PotPlayerProbeStatus::InputStateUnavailable, GetLastError());
    if (finalGui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))
        return reject(PotPlayerProbeStatus::MenuActive);
    if (finalGui.flags & GUI_INMOVESIZE) return reject(PotPlayerProbeStatus::MoveSizeActive);
    result.status = PotPlayerProbeStatus::Ready;
    return result;
}

PotPlayerVideoProbeResult probeForegroundPotPlayerVideoWindow() noexcept {
    return probePotPlayerVideoWindow(GetAncestor(GetForegroundWindow(), GA_ROOT));
}

const char* potPlayerProbeStatusName(PotPlayerProbeStatus status) noexcept {
    switch (status) {
    case PotPlayerProbeStatus::Ready: return "ready";
    case PotPlayerProbeStatus::NoWindow: return "no-window";
    case PotPlayerProbeStatus::InvalidWindow: return "invalid-window";
    case PotPlayerProbeStatus::NotTopLevel: return "not-top-level";
    case PotPlayerProbeStatus::OwnProcess: return "own-process";
    case PotPlayerProbeStatus::ProcessMismatch: return "process-mismatch";
    case PotPlayerProbeStatus::IdentityUnavailable: return "identity-unavailable";
    case PotPlayerProbeStatus::UnsupportedIdentity: return "unsupported-identity";
    case PotPlayerProbeStatus::Hidden: return "hidden";
    case PotPlayerProbeStatus::Minimized: return "minimized";
    case PotPlayerProbeStatus::Cloaked: return "cloaked";
    case PotPlayerProbeStatus::Disabled: return "disabled";
    case PotPlayerProbeStatus::NotForeground: return "not-foreground";
    case PotPlayerProbeStatus::InputStateUnavailable: return "input-state-unavailable";
    case PotPlayerProbeStatus::MenuActive: return "menu-active";
    case PotPlayerProbeStatus::MoveSizeActive: return "move-size-active";
    case PotPlayerProbeStatus::DpiContextUnavailable: return "dpi-context-unavailable";
    case PotPlayerProbeStatus::GeometryUnavailable: return "geometry-unavailable";
    case PotPlayerProbeStatus::VideoCanvasNotFound: return "video-canvas-not-found";
    case PotPlayerProbeStatus::WindowChanged: return "window-changed";
    }
    return "unknown";
}

} // namespace veyra::capture

