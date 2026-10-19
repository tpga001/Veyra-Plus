#include "veyra/capture/PotPlayerVideoProbe.h"

#include <dwmapi.h>
#include <array>
#include <cstdint>
#include <cerrno>
#include <cwchar>
#include <string>
#include <iostream>
#include <limits>
#include <string_view>


namespace {
void printRect(std::ostream& out, const RECT& rect) {
    out << '[' << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom << ']';
}

bool permitsDiagnosticGeometry(veyra::capture::PotPlayerProbeStatus status) {
    using Status = veyra::capture::PotPlayerProbeStatus;
    switch (status) {
    case Status::Ready:
    case Status::Hidden:
    case Status::Minimized:
    case Status::Cloaked:
    case Status::Disabled:
    case Status::NotForeground:
    case Status::InputStateUnavailable:
    case Status::MenuActive:
    case Status::MoveSizeActive:
    case Status::DpiContextUnavailable:
    case Status::GeometryUnavailable:
    case Status::VideoCanvasNotFound:
        return true;
    default:
        return false;
    }
}

void printDiagnosticGeometry(const veyra::capture::PotPlayerVideoProbeResult& result) {
    // Inspection-only fallback; it never changes product probe readiness.
    // Read only a process whose PotPlayer identity was verified by the probe.
    if (!permitsDiagnosticGeometry(result.status)) { std::cout << "null"; return; }
    const HWND player = result.geometry.playerWindow;
    auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!previous) { std::cout << "null"; return; }
    RECT window{}, client{}, frame{};
    const bool hasWindow = GetWindowRect(player, &window) != FALSE;
    const bool hasClient = GetClientRect(player, &client) != FALSE;
    POINT corners[2]{{client.left, client.top}, {client.right, client.bottom}};
    const bool physicalClient = hasClient && ClientToScreen(player, &corners[0]) &&
        ClientToScreen(player, &corners[1]);
    if (physicalClient) client = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
    const bool hasFrame = SUCCEEDED(DwmGetWindowAttribute(player, DWMWA_EXTENDED_FRAME_BOUNDS,
        &frame, sizeof(frame)));
    std::cout << "{\"captureAllowed\":false,\"dpi\":" << GetDpiForWindow(player)
        << ",\"windowRect\":";
    if (hasWindow) printRect(std::cout, window); else std::cout << "null";
    std::cout << ",\"clientRect\":";
    if (physicalClient) printRect(std::cout, client); else std::cout << "null";
    std::cout << ",\"extendedFrameRect\":";
    if (hasFrame) printRect(std::cout, frame); else std::cout << "null";
    std::cout << ",\"videoClassCandidates\":[";
    bool first = true;
    EnumChildWindows(player, [](HWND child, LPARAM context)->BOOL {
        auto& first = *reinterpret_cast<bool*>(context);
        wchar_t name[64]{};
        if (GetClassNameW(child, name, 64) <= 0 || std::wstring_view(name) != L"AfxWnd140su")
            return TRUE;
        RECT rect{};
        if (!GetWindowRect(child, &rect)) return TRUE;
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"hwnd\":" << reinterpret_cast<uintptr_t>(child)
            << ",\"class\":\"AfxWnd140su\",\"visible\":"
            << (IsWindowVisible(child) ? "true" : "false") << ",\"rect\":";
        printRect(std::cout, rect);
        std::cout << '}';
        return TRUE;
    }, reinterpret_cast<LPARAM>(&first));
    std::cout << "]}";
    SetThreadDpiAwarenessContext(previous);
}

void printProbe(const veyra::capture::PotPlayerVideoProbeResult& result) {
    using namespace veyra::capture;
    const auto& geometry = result.geometry;
    std::cout << "{\"status\":\"" << potPlayerProbeStatusName(result.status)
        << "\",\"ready\":" << (result.ready() ? "true" : "false")
        << ",\"win32Error\":" << result.win32Error << ",\"pid\":" << geometry.processId
        << ",\"playerWindow\":" << reinterpret_cast<uintptr_t>(geometry.playerWindow)
        << ",\"videoWindow\":" << reinterpret_cast<uintptr_t>(geometry.videoWindow)
        << ",\"monitor\":" << reinterpret_cast<uintptr_t>(geometry.monitor)
        << ",\"dpi\":" << geometry.dpi
        << ",\"videoRectIsHeuristic\":" << (geometry.videoRectIsHeuristic ? "true" : "false")
        << ",\"windowRect\":";
    printRect(std::cout, geometry.windowRect);
    std::cout << ",\"clientRect\":";
    printRect(std::cout, geometry.clientRect);
    std::cout << ",\"extendedFrameRect\":";
    printRect(std::cout, geometry.extendedFrameRect);
    std::cout << ",\"videoRect\":";
    printRect(std::cout, geometry.videoRect);
    std::cout << ",\"diagnosticOnly\":";
    printDiagnosticGeometry(result);
    std::cout << '}';
}

int inspect(int argc, wchar_t** argv) {
    using namespace veyra::capture;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--inspect-foreground") {
        printProbe(probeForegroundPotPlayerVideoWindow());
        std::cout << '\n';
        return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--inspect-hwnd") {
        wchar_t* end = nullptr;
        errno = 0;
        const auto value = std::wcstoull(argv[2], &end, 0);
        if (errno || !end || end == argv[2] || *end || argv[2][0] == L'-') return 2;
        printProbe(probePotPlayerVideoWindow(reinterpret_cast<HWND>(static_cast<uintptr_t>(value))));
        std::cout << '\n';
        return 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--list-potplayer-windows") {
        std::cout << '[';
        bool first = true;
        EnumWindows([](HWND window, LPARAM context)->BOOL {
            wchar_t name[64]{};
            if (GetClassNameW(window, name, 64) <= 0 || std::wstring_view(name) != L"PotPlayer64")
                return TRUE;
            auto& first = *reinterpret_cast<bool*>(context);
            if (!first) std::cout << ',';
            first = false;
            printProbe(probePotPlayerVideoWindow(window));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&first));
        std::cout << "]\n";
        return 0;
    }
    std::cerr << "Usage: PotPlayerVideoProbeTests [--inspect-foreground | --inspect-hwnd HANDLE | --list-potplayer-windows]\n";
    return 2;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) return inspect(argc, argv);
    using namespace veyra::capture;
    int failures = 0;
    unsigned checks = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failures; std::cerr << "FAIL " << name << '\n'; }
    };
    auto handle = [](uintptr_t id) { return reinterpret_cast<HWND>(id); };
    auto sameRect = [](const RECT& a, const RECT& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    };
    const RECT client{-1920, 100, -20, 1100};
    std::array<PotPlayerCanvasCandidate, 5> candidates{{
        {handle(1), {-1910, 110, -30, 1000}, true, true},
        {handle(2), {-1920, 100, -20, 1100}, false, true},
        {handle(3), {-1920, 100, -20, 1100}, true, false},
        {handle(4), {-1800, 250, -1700, 330}, true, true},
        {handle(5), {-1800, 200, -1750, 1000}, true, true}
    }};
    auto selected = selectPotPlayerVideoCanvas(candidates, client);
    check(selected && selected->window == handle(1), "largest visible matching canvas only");
    check(selected && sameRect(selected->physicalScreenRect, candidates[0].physicalScreenRect),
        "physical negative monitor coordinates are preserved without DPI rescaling");
    candidates[0].physicalScreenRect = {-2000, 50, 50, 1200};
    selected = selectPotPlayerVideoCanvas(candidates, client);
    check(selected && sameRect(selected->physicalScreenRect, client),
        "canvas cannot capture outside the player client area");
    candidates[0].physicalScreenRect = {0, 0, 1920, 1080};
    selected = selectPotPlayerVideoCanvas(candidates, client);
    check(selected && selected->window == handle(4), "off-window canvas excluded");
    candidates[3].physicalScreenRect = {-1900, 200, -1836, 264};
    selected = selectPotPlayerVideoCanvas(candidates, client);
    check(selected && selected->window == handle(4), "64 by 64 boundary accepted");
    candidates[3].physicalScreenRect.bottom = 263;
    check(!selectPotPlayerVideoCanvas(candidates, client), "clipped canvas below 64 rejected");
    candidates[3].physicalScreenRect = {-30, 200, 800, 800};
    check(!selectPotPlayerVideoCanvas(candidates, client), "tiny client intersection rejected");
    check(!selectPotPlayerVideoCanvas(candidates, RECT{}), "invalid client geometry rejected");
    check(!selectPotPlayerVideoCanvas({}, client), "missing video canvas never falls back to whole window");
    candidates[3] = {nullptr, client, true, true};
    check(!selectPotPlayerVideoCanvas(candidates, client), "null canvas handle rejected");
    candidates[3] = {handle(4), {-40, 200, -80, 600}, true, true};
    check(!selectPotPlayerVideoCanvas(candidates, client), "inverted video rectangle rejected");
    const LONG lo = (std::numeric_limits<LONG>::min)(), hi = (std::numeric_limits<LONG>::max)();
    std::array<PotPlayerCanvasCandidate, 2> large{{
        {handle(8), {lo, lo, hi, hi}, true, true},
        {handle(9), {0, 0, hi, hi}, true, true}
    }};
    selected = selectPotPlayerVideoCanvas(large, RECT{lo, lo, hi, hi});
    check(selected && selected->window == handle(8), "large coordinate area uses non-overflowing arithmetic");

    const auto absent = probePotPlayerVideoWindow(nullptr);
    check(!absent.ready() && absent.status == PotPlayerProbeStatus::NoWindow,
        "null player is not a usable source");
    check(!probePotPlayerVideoWindow(handle(1)).ready(), "invalid window is not usable");
    check(!probePotPlayerVideoWindow(GetDesktopWindow()).ready(), "desktop cannot be a player source");
    check(std::string_view(potPlayerProbeStatusName(PotPlayerProbeStatus::MoveSizeActive)) == "move-size-active",
        "unsafe move/resize status is observable");
    check(std::string_view(potPlayerProbeStatusName(PotPlayerProbeStatus::MenuActive)) == "menu-active",
        "menu avoidance is observable");
    check(std::string_view(potPlayerProbeStatusName(PotPlayerProbeStatus::Ready)) == "ready",
        "metadata readiness has an explicit status");

    // No visible test window or player process is created. This hidden fixture
    // checks process/session identity and self-capture rejection via real Win32.
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VeyraPotPlayerProbeUnitFixture";
    const ATOM atom = RegisterClassW(&wc);
    const HWND fixture = atom ? CreateWindowExW(0, wc.lpszClassName, nullptr, WS_POPUP,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr) : nullptr;
    check(fixture != nullptr, "hidden identity fixture created");
    if (fixture) {
        check(probePotPlayerVideoWindow(fixture).status == PotPlayerProbeStatus::OwnProcess,
            "self-capture rejected even when a source HWND exists");
        DWORD mismatched = GetCurrentProcessId() + 1;
        if (!mismatched) mismatched = 1;
        check(probePotPlayerVideoWindow(fixture, mismatched).status == PotPlayerProbeStatus::ProcessMismatch,
            "expected process pins the HWND to a session");
        const HWND child = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD, 0, 0, 64, 64,
            fixture, nullptr, wc.hInstance, nullptr);
        check(child && probePotPlayerVideoWindow(child).status == PotPlayerProbeStatus::NotTopLevel,
            "child windows cannot be selected as the player root");
        if (child) DestroyWindow(child);
        DestroyWindow(fixture);
        check(probePotPlayerVideoWindow(fixture).status == PotPlayerProbeStatus::InvalidWindow,
            "destroyed player HWND is rejected");
    }
    if (atom) UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::cout << "PotPlayer video probe checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}

