#pragma once

#include <windows.h>
#include <optional>
#include <span>

namespace veyra::capture {

// Ready describes safe window metadata only. It does not prove active playback,
// usable captured pixels, source-media timestamps, or enhancement execution.
enum class PotPlayerProbeStatus {
    Ready,
    NoWindow,
    InvalidWindow,
    NotTopLevel,
    OwnProcess,
    ProcessMismatch,
    IdentityUnavailable,
    UnsupportedIdentity,
    Hidden,
    Minimized,
    Cloaked,
    Disabled,
    NotForeground,
    InputStateUnavailable,
    MenuActive,
    MoveSizeActive,
    DpiContextUnavailable,
    GeometryUnavailable,
    VideoCanvasNotFound,
    WindowChanged
};

struct PotPlayerVideoGeometry {
    HWND playerWindow = nullptr;
    HWND videoWindow = nullptr;
    DWORD processId = 0;
    UINT dpi = 0;
    HMONITOR monitor = nullptr;
    // All rectangles use physical screen pixels, including negative coordinates.
    RECT windowRect{};
    RECT clientRect{};
    RECT extendedFrameRect{};
    RECT videoRect{};
    // AfxWnd140su is a tested skin/version heuristic, not a PotPlayer public API.
    // The canvas may contain black bars, burned-in text or floating player OSD.
    bool videoRectIsHeuristic = true;
};

struct PotPlayerVideoProbeResult {
    PotPlayerProbeStatus status = PotPlayerProbeStatus::NoWindow;
    PotPlayerVideoGeometry geometry{};
    DWORD win32Error = ERROR_SUCCESS;
    bool ready() const noexcept { return status == PotPlayerProbeStatus::Ready; }
};

// Read-only window inspection; no player commands, injection, file-title queries,
// capture session, GPU work or fallback to whole-window/desktop capture.
// The supported identity is PotPlayer64 + PotPlayerMini64.exe. expectedProcessId
// pins a session to its process and detects a destroyed/reused window handle.
PotPlayerVideoProbeResult probePotPlayerVideoWindow(
    HWND window, DWORD expectedProcessId = 0) noexcept;
PotPlayerVideoProbeResult probeForegroundPotPlayerVideoWindow() noexcept;
const char* potPlayerProbeStatusName(PotPlayerProbeStatus status) noexcept;

// Pure selection over window observations, also usable by future skin adapters.
// Enumeration must already establish child/root/PID ownership. A candidate is
// clipped to the player's client area; only visible matching canvases >=64x64
// are eligible. No candidate means no safe video-region capture.
struct PotPlayerCanvasCandidate {
    HWND window = nullptr;
    RECT physicalScreenRect{};
    bool visible = false;
    bool matchesVideoClass = false;
};
std::optional<PotPlayerCanvasCandidate> selectPotPlayerVideoCanvas(
    std::span<const PotPlayerCanvasCandidate> candidates,
    const RECT& playerClientRect) noexcept;

} // namespace veyra::capture

