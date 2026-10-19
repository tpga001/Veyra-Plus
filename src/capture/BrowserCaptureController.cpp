#include "veyra/capture/BrowserCaptureController.h"
#include "veyra/engine/BrowserRtxInputPolicy.h"
#include "veyra/capture/BrowserAttachmentPolicy.h"
#include "veyra/capture/VideoRegionOverlay.h"
#include "veyra/source/ScreenCaptureSource.h"
#include "veyra/engine/NativePlaybackPresets.h"
#include "veyra/engine/PlaybackDisplayPolicy.h"
#include "veyra/Log.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QHash>
#include <QSet>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <format>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

namespace veyra::capture {
namespace {
// Slow callback evidence only; elapsed is captured before any log lock.
class BrowserCallbackTimingProbe final {
    const char* fixedStage_;
    ULONGLONG began_;
public:
    explicit BrowserCallbackTimingProbe(const char* fixedStage)
        : fixedStage_(fixedStage), began_(GetTickCount64()) {}
    ~BrowserCallbackTimingProbe() {
        const auto elapsed = GetTickCount64() - began_;
        if (elapsed <= 100) return;
        const auto error = GetLastError();
        log::warn("browser-gui-probe", std::format("stage={} elapsedMs={}", fixedStage_, elapsed));
        SetLastError(error);
    }
};
constexpr quint16 kPort = 24861;
constexpr qsizetype kBodyLimit = 64 * 1024, kHeaderLimit = 8192;
constexpr ULONGLONG kReportExpiryMs = 2500, kStableMs = 350;
constexpr int kConnectionLimit = 8, kMessagesPerSecond = 64;
bool positiveSize(const RECT& r) { return r.right > r.left && r.bottom > r.top; }
LONG width(const RECT& r) { return r.right - r.left; }
LONG height(const RECT& r) { return r.bottom - r.top; }
bool sameRect(const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }
bool finiteNumber(const QJsonObject& object, const char* name, double& result,
                  double minimum, double maximum) {
    const auto value = object.value(QLatin1String(name));
    if (!value.isDouble()) return false;
    result = value.toDouble();
    return std::isfinite(result) && result >= minimum && result <= maximum;
}
bool identifier(const QJsonObject& object, const char* name, QString& result) {
    const auto value = object.value(QLatin1String(name));
    if (!value.isString()) return false;
    result = value.toString();
    return !result.isEmpty() && result.size() <= 128 &&
        !result.contains(QChar::Null) && !result.contains(QLatin1Char('\r')) &&
        !result.contains(QLatin1Char('\n'));
}
QString ownerKey(const QString& origin, const QString& client, const QString& session) {
    return origin + QChar(0x1f) + client + QChar(0x1f) + session;
}
struct BrowserBinding {
    HWND browser = nullptr, viewport = nullptr;
    DWORD processId = 0;
    ULONGLONG processCreatedAt = 0;
    policy::BrowserIdentity identity() const {
        return {reinterpret_cast<std::uintptr_t>(browser), processId, processCreatedAt};
    }
};
struct Report {
    QString origin, client, session;
    QString stopReason;
    quint64 sequence = 0;
    double left = 0, top = 0, width = 0, height = 0;
    double viewportWidth = 0, viewportHeight = 0, dpr = 1, frameRate = 0;
    double screenX = 0, screenY = 0, outerWidth = 0, outerHeight = 0;
    bool hasOuterGeometry = false, playing = false;
    unsigned sourceWidth = 0, sourceHeight = 0;
    policy::MediaFingerprint media;
    std::vector<std::array<double, 4>> holes;
    QJsonObject windowInfo;
    BrowserBinding binding;
    ULONGLONG receivedAt = 0;
    QString key() const { return ownerKey(origin, client, session); }
    QString documentKey() const {
        return QString::number(windowInfo.value("browserWindowId").toInt(-1)) + QChar(0x1f) +
            QString::number(windowInfo.value("tabId").toInt(-1)) + QChar(0x1f) +
            windowInfo.value("documentId").toString();
    }
    QString bindingKey() const { return key() + QChar(0x1e) + documentKey(); }
};
BrowserTelemetrySource telemetryIdentity(const Report& report) {
    BrowserTelemetrySource source;
    source.available = report.binding.identity().valid();
    source.document = report.bindingKey().toStdString();
    source.browser = report.binding.identity(); source.media = report.media;
    return source;
}
struct Geometry {
    HWND browser = nullptr, viewport = nullptr;
    DWORD processId = 0;
    ULONGLONG processCreatedAt = 0;
    UINT dpi = 0;
    HMONITOR monitor = nullptr;
    RECT frame{}, video{};
    pipeline::Extent display{};
    policy::MediaFingerprint media;
    policy::OuterBoundsMatch outerMatch = policy::OuterBoundsMatch::Mismatch;
};
bool sameCapture(const Geometry& a, const Geometry& b) {
    return a.browser == b.browser && a.viewport == b.viewport && a.processId == b.processId &&
        a.processCreatedAt == b.processCreatedAt &&
        a.dpi == b.dpi && a.monitor == b.monitor && a.display == b.display &&
        policy::sameMedia(a.media, b.media) &&
        width(a.frame) == width(b.frame) && height(a.frame) == height(b.frame) &&
        width(a.video) == width(b.video) && height(a.video) == height(b.video) &&
        a.video.left - a.frame.left == b.video.left - b.frame.left &&
        a.video.top - a.frame.top == b.video.top - b.frame.top;
}
class PhysicalCoordinates {
public:
    PhysicalCoordinates() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalCoordinates() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
    bool valid() const { return previous_ != nullptr; }
private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};
bool supportedBrowser(DWORD pid, ULONGLONG& createdAt) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[32768]{}; DWORD size = DWORD(std::size(path));
    const BOOL queried = QueryFullProcessImageNameW(process, 0, path, &size);
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL timed = GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    if (!queried || !timed) return false;
    createdAt = (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    const wchar_t* basename = wcsrchr(path, L'\\'); basename = basename ? basename + 1 : path;
    return _wcsicmp(basename, L"chrome.exe") == 0 || _wcsicmp(basename, L"msedge.exe") == 0;
}
struct ViewportSearch { HWND root = nullptr; int expectedWidth = 0, expectedHeight = 0; std::vector<HWND> matches; };
BOOL CALLBACK findViewport(HWND child, LPARAM parameter) {
    auto& search = *reinterpret_cast<ViewportSearch*>(parameter);
    wchar_t name[80]{};
    if (!IsWindowVisible(child) || GetAncestor(child, GA_ROOT) != search.root ||
        !GetClassNameW(child, name, int(std::size(name))) ||
        wcscmp(name, L"Chrome_RenderWidgetHostHWND") != 0) return TRUE;
    RECT client{};
    if (GetClientRect(child, &client) &&
        std::abs(width(client) - search.expectedWidth) <= 2 &&
        std::abs(height(client) - search.expectedHeight) <= 2) search.matches.push_back(child);
    return TRUE;
}
std::uintptr_t rootedViewportHitOwner(HWND root, POINT screenPoint, const std::vector<HWND>& candidates) {
    HWND current = root;
    for (unsigned depth = 0; depth < 16; ++depth) {
        if (!IsWindow(current) || GetAncestor(current, GA_ROOT) != root) return 0;
        POINT local = screenPoint;
        RECT client{};
        if (!ScreenToClient(current, &local) || !GetClientRect(current, &client) ||
            !PtInRect(&client, local)) return 0;
        // Chromium's live content widget can itself be WS_EX_TRANSPARENT.
        // Use only this browser's child z-order, including transparent widgets;
        // the separate existing visibility policy still checks outside occlusion.
        const HWND hit = ChildWindowFromPointEx(current, local, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
        if (!hit || GetAncestor(hit, GA_ROOT) != root) return 0;
        if (hit != current) { current = hit; continue; }
        if (hit == root) return 0;
        std::uintptr_t owner = 0;
        for (const HWND candidate : candidates) {
            if (!IsWindow(candidate) || GetAncestor(candidate, GA_ROOT) != root) return 0;
            if (hit != candidate && !IsChild(candidate, hit)) continue;
            if (owner) return 0; // Nested candidates cannot uniquely own this hit.
            owner = reinterpret_cast<std::uintptr_t>(candidate);
        }
        return owner;
    }
    return 0; // Fail closed if a child hierarchy exceeds the bounded traversal.
}
HWND selectViewport(const ViewportSearch& search) {
    std::vector<std::uintptr_t> candidates;
    for (const HWND candidate : search.matches)
        candidates.push_back(reinterpret_cast<std::uintptr_t>(candidate));
    if (search.matches.size() <= 1)
        return reinterpret_cast<HWND>(policy::selectViewportCandidate(candidates, {}));
    RECT common{};
    bool first = true;
    for (const HWND candidate : search.matches) {
        RECT client{};
        if (!IsWindow(candidate) || GetAncestor(candidate, GA_ROOT) != search.root ||
            !GetClientRect(candidate, &client)) return nullptr;
        POINT topLeft{client.left, client.top}, bottomRight{client.right, client.bottom};
        if (!ClientToScreen(candidate, &topLeft) || !ClientToScreen(candidate, &bottomRight)) return nullptr;
        const RECT screen{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        if (!positiveSize(screen)) return nullptr;
        if (first) { common = screen; first = false; }
        else {
            RECT clipped{};
            if (!IntersectRect(&clipped, &common, &screen)) return nullptr;
            common = clipped;
        }
    }
    // Three distinct points strictly inside every candidate, never on an edge.
    if (width(common) < 4 || height(common) < 4) return nullptr;
    std::vector<std::uintptr_t> hitOwners;
    for (LONG part = 1; part <= 3; ++part) {
        const POINT point{common.left + width(common) * part / 4,
                          common.top + height(common) * part / 4};
        hitOwners.push_back(rootedViewportHitOwner(search.root, point, search.matches));
    }
    return reinterpret_cast<HWND>(policy::selectViewportCandidate(candidates, hitOwners));
}
std::optional<Geometry> geometryFor(const Report& report, QString& reason, QString& reasonCode,
                                     bool freshMetadata = false, HWND monitorWindow = nullptr,
                                     HWND overlayWindow = nullptr, const Report* monitorAuthority = nullptr) {
    auto reject = [&](const QString& message, const char* code = "geometry-unavailable") -> std::optional<Geometry> {
        reason = message; reasonCode = QLatin1String(code); return {};
    };
    Geometry geometry;
    geometry.media = report.media;
    const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
    geometry.browser = report.binding.browser ? report.binding.browser : foreground;
    const bool ownMonitor = report.binding.browser && VideoRegionOverlay::knownMonitorForeground(monitorWindow, overlayWindow);
    if (geometry.browser != foreground && !ownMonitor)
        return reject(QStringLiteral("已绑定的视频窗口不在前台；等待该窗口的新报告"), "foreground-mismatch");
    if (!geometry.browser || !IsWindow(geometry.browser) || !IsWindowVisible(geometry.browser) ||
        IsIconic(geometry.browser) || !IsWindowEnabled(geometry.browser))
        return reject(QStringLiteral("等待前台浏览器视频；原画保持显示"), "waiting-video");
    const DWORD thread = GetWindowThreadProcessId(geometry.browser, &geometry.processId);
    if (!thread || !geometry.processId || geometry.processId == GetCurrentProcessId() ||
        !supportedBrowser(geometry.processId, geometry.processCreatedAt))
        return reject(QStringLiteral("等待 Chrome 或 Edge 回到前台"), "foreground-mismatch");
    if (report.binding.browser &&
        (geometry.processId != report.binding.processId ||
         geometry.processCreatedAt != report.binding.processCreatedAt))
        return reject(QStringLiteral("已绑定的浏览器实例已变化；等待新的网页来源"), "browser-identity-changed");
    const QString pageTitle = report.windowInfo.value("pageTitle").toString().trimmed();
    const int titleLength = GetWindowTextLengthW(geometry.browser);
    if (pageTitle.isEmpty() || pageTitle.size() > 4096 || titleLength <= 0 || titleLength > 8192)
        return reject(QStringLiteral("网页标题暂不可验证；保留原画"), "page-title-unavailable");
    std::vector<wchar_t> titleBuffer(size_t(titleLength) + 1);
    if (GetWindowTextW(geometry.browser, titleBuffer.data(), int(titleBuffer.size())) <= 0 ||
        !QString::fromWCharArray(titleBuffer.data()).contains(pageTitle))
        return reject(QStringLiteral("网页标题与前台浏览器不匹配；保留原画"), "page-title-mismatch");
    if (GetWindowLongPtrW(geometry.browser, GWL_EXSTYLE) & WS_EX_TOPMOST)
        return reject(QStringLiteral("浏览器处于置顶状态；保留原画，不提升增强窗口"));
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(geometry.browser, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked)
        return reject(QStringLiteral("浏览器当前不可见；原画保持显示"));
    GUITHREADINFO gui{sizeof(gui)};
    if (!GetGUIThreadInfo(thread, &gui) ||
        (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE)))
        return reject(QStringLiteral("等待浏览器菜单或窗口调整结束"));
    PhysicalCoordinates physical;
    if (!physical.valid()) return reject(QStringLiteral("无法读取物理坐标；保留原画"));
    geometry.dpi = GetDpiForWindow(geometry.browser);
    if (!geometry.dpi || FAILED(DwmGetWindowAttribute(geometry.browser, DWMWA_EXTENDED_FRAME_BOUNDS,
        &geometry.frame, sizeof(geometry.frame))) || !positiveSize(geometry.frame))
        return reject(QStringLiteral("浏览器捕获边界不可用；保留原画"));
    RECT outer{};
    if (!GetWindowRect(geometry.browser, &outer)) return reject(QStringLiteral("浏览器窗口位置不可用"));
    // Win32 includes invisible resize borders; DWM returns the visible frame in
    // physical coordinates. Match all four edges against one actual rectangle,
    // with rounding tolerance only. Never guess mixed-DPI monitor origins.
    if (report.hasOuterGeometry) {
        const double scale = double(geometry.dpi) / 96.0;
        const auto bounds = [](const RECT& value) -> policy::Bounds {
            return {double(value.left), double(value.top), double(value.right), double(value.bottom)};
        };
        const policy::Bounds reported{report.screenX, report.screenY,
            report.screenX + report.outerWidth, report.screenY + report.outerHeight};
        geometry.outerMatch = policy::outerBoundsMatch(reported, bounds(outer), bounds(geometry.frame), scale);
        if (geometry.outerMatch == policy::OuterBoundsMatch::Mismatch) {
            const auto sizeMatches = [&](const RECT& actual, double factor) {
                return std::abs(report.outerWidth * factor - width(actual)) <= 2 &&
                    std::abs(report.outerHeight * factor - height(actual)) <= 2;
            };
            const bool matchedSize = sizeMatches(outer, 1) || sizeMatches(outer, scale) ||
                sizeMatches(geometry.frame, 1) || sizeMatches(geometry.frame, scale);
            return reject(QStringLiteral("视频报告与窗口实际边界不匹配；混合缩放坐标尚未验证，保留原画"),
                matchedSize ? "window-origin-mismatch" : "window-bounds-mismatch");
        }
    }
    ViewportSearch search{geometry.browser, int(std::lround(report.viewportWidth * report.dpr)),
        int(std::lround(report.viewportHeight * report.dpr)), {}};
    EnumChildWindows(geometry.browser, findViewport, reinterpret_cast<LPARAM>(&search));
    geometry.viewport = selectViewport(search);
    if (!geometry.viewport)
        return reject(QStringLiteral("无法唯一匹配网页视频坐标；保留原画"), "viewport-unmatched");
    const auto viewportDecision = policy::viewportDecision(report.binding.identity(),
        {reinterpret_cast<std::uintptr_t>(geometry.browser), geometry.processId, geometry.processCreatedAt},
        reinterpret_cast<std::uintptr_t>(report.binding.viewport), reinterpret_cast<std::uintptr_t>(geometry.viewport),
        report.binding.viewport && IsWindow(report.binding.viewport), freshMetadata);
    if (viewportDecision == policy::ViewportDecision::RejectBrowser)
        return reject(QStringLiteral("已绑定的浏览器实例已变化；等待新的网页来源"), "browser-identity-changed");
    if (viewportDecision == policy::ViewportDecision::RejectViewport)
        return reject(QStringLiteral("已绑定的网页视口已变化；等待该来源的有效新报告"), "viewport-changed");
    RECT client{}; POINT origin{};
    if (!GetClientRect(geometry.viewport, &client) || !ClientToScreen(geometry.viewport, &origin))
        return reject(QStringLiteral("网页内容原点不可用；保留原画"));
    geometry.video = {
        origin.x + LONG(std::lround(report.left * report.dpr)),
        origin.y + LONG(std::lround(report.top * report.dpr)),
        origin.x + LONG(std::lround((report.left + report.width) * report.dpr)),
        origin.y + LONG(std::lround((report.top + report.height) * report.dpr))};
    const RECT viewport{origin.x, origin.y, origin.x + width(client), origin.y + height(client)};
    RECT clipped{};
    if (!IntersectRect(&clipped, &geometry.video, &viewport) || !sameRect(clipped, geometry.video) ||
        !IntersectRect(&clipped, &geometry.video, &geometry.frame) || !sameRect(clipped, geometry.video) ||
        width(geometry.video) < 64 || height(geometry.video) < 64 ||
        width(geometry.video) > 16384 || height(geometry.video) > 16384)
        return reject(QStringLiteral("视频矩形超出捕获边界；保留原画"));
    geometry.monitor = MonitorFromRect(&geometry.video, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{sizeof(monitor)};
    if (!geometry.monitor || !GetMonitorInfoW(geometry.monitor, &monitor))
        return reject(QStringLiteral("视频所在屏幕不可用"));
    geometry.display = {unsigned(width(monitor.rcMonitor)), unsigned(height(monitor.rcMonitor))};
    DWORD finalPid = 0; GetWindowThreadProcessId(geometry.browser, &finalPid);
    const bool regionAllowed = VideoRegionOverlay::foregroundRegionAllowed(geometry.browser, monitorWindow, overlayWindow, geometry.video);
    const bool monitorForegroundAllowed = VideoRegionOverlay::knownMonitorForeground(monitorWindow, overlayWindow);
    const HWND finalForeground = GetAncestor(GetForegroundWindow(), GA_ROOT);
    const bool finalBrowserForeground = finalForeground == geometry.browser;
    const bool finalOwnMonitor = monitorForegroundAllowed && (finalForeground == monitorWindow ||
        (finalForeground && GetAncestor(finalForeground, GA_ROOTOWNER) == monitorWindow));
    const bool trustedOwnMonitor = finalOwnMonitor && monitorAuthority &&
        browserMonitorResumeAllowed(telemetryIdentity(*monitorAuthority), telemetryIdentity(report), finalOwnMonitor, regionAllowed);
    const bool reportedFocused = !report.windowInfo.contains("focused") || report.windowInfo.value("focused").toBool();
    if (!IsWindow(geometry.viewport) || finalPid != geometry.processId ||
        GetAncestor(geometry.viewport, GA_ROOT) != geometry.browser ||
        !regionAllowed || !browserFinalForegroundAllowed(finalBrowserForeground, finalOwnMonitor) ||
        (finalOwnMonitor && !trustedOwnMonitor) || !browserReportedFocusAllowed(reportedFocused, trustedOwnMonitor))
        return reject(QStringLiteral("前台窗口已变化；保留原画"), "foreground-mismatch");
    reason.clear(); reasonCode.clear(); return geometry;
}
bool parseReport(const QJsonObject& object, const QString& origin, Report& report, bool& stop, QString& error) {
    auto reject = [&](const QString& message) { error = message; return false; };
    if (!object.value("schema").isDouble() || object.value("schema").toDouble() != 1 ||
        !identifier(object, "clientId", report.client) ||
        !identifier(object, "sessionId", report.session)) return reject(QStringLiteral("invalid report identity"));
    double sequence = 0;
    if (!finiteNumber(object, "sequence", sequence, 1, 9007199254740991.0) || std::floor(sequence) != sequence)
        return reject(QStringLiteral("invalid sequence"));
    report.origin = origin; report.sequence = quint64(sequence);
    const QString type = object.value("type").toString();
    stop = type == QLatin1String("stop");
    if (stop) { report.stopReason = object.value("reason").toString().left(128); return true; }
    if (type != QLatin1String("video") || !object.value("playing").isBool())
        return reject(QStringLiteral("invalid report type"));
    report.playing = object.value("playing").toBool();
    const auto rect = object.value("rect").toObject(), viewport = object.value("viewport").toObject();
    if (!finiteNumber(rect, "left", report.left, 0, 16384) ||
        !finiteNumber(rect, "top", report.top, 0, 16384) ||
        !finiteNumber(rect, "width", report.width, 1, 16384) ||
        !finiteNumber(rect, "height", report.height, 1, 16384)) return reject(QStringLiteral("invalid video rectangle"));
    const char* widthKey = viewport.contains("innerWidth") ? "innerWidth" : "width";
    const char* heightKey = viewport.contains("innerHeight") ? "innerHeight" : "height";
    if (!finiteNumber(viewport, widthKey, report.viewportWidth, 1, 16384) ||
        !finiteNumber(viewport, heightKey, report.viewportHeight, 1, 16384) ||
        !finiteNumber(viewport, "devicePixelRatio", report.dpr, .25, 8) ||
        report.viewportWidth * report.dpr > 16384 || report.viewportHeight * report.dpr > 16384 ||
        report.left + report.width > report.viewportWidth + .1 ||
        report.top + report.height > report.viewportHeight + .1) return reject(QStringLiteral("invalid viewport"));
    const auto visual = viewport.value("visualViewport");
    if (!visual.isUndefined()) {
        if (!visual.isObject()) return reject(QStringLiteral("invalid visual viewport"));
        double scale = 1, x = 0, y = 0;
        if (!finiteNumber(visual.toObject(), "scale", scale, .1, 10) ||
            !finiteNumber(visual.toObject(), "offsetLeft", x, -16384, 16384) ||
            !finiteNumber(visual.toObject(), "offsetTop", y, -16384, 16384) ||
            std::abs(scale - 1) > .001 || std::abs(x) > .1 || std::abs(y) > .1)
            return reject(QStringLiteral("unsupported visual viewport transform"));
    }
    double pageZoom = 1;
    if (viewport.contains("pageZoom") && !finiteNumber(viewport, "pageZoom", pageZoom, .25, 5))
        return reject(QStringLiteral("invalid page zoom"));
    const unsigned outerFieldCount = unsigned(viewport.contains("screenX")) + unsigned(viewport.contains("screenY")) +
        unsigned(viewport.contains("outerWidth")) + unsigned(viewport.contains("outerHeight"));
    report.hasOuterGeometry = finiteNumber(viewport, "screenX", report.screenX, -131072, 131072) &&
        finiteNumber(viewport, "screenY", report.screenY, -131072, 131072) &&
        finiteNumber(viewport, "outerWidth", report.outerWidth, 1, 32768) &&
        finiteNumber(viewport, "outerHeight", report.outerHeight, 1, 32768);
    if (outerFieldCount && (outerFieldCount != 4 || !report.hasOuterGeometry))
        return reject(QStringLiteral("invalid outer window geometry"));
    report.windowInfo = object.value("windowInfo").toObject();
    if (report.windowInfo.contains("focused") && !report.windowInfo.value("focused").isBool())
        return reject(QStringLiteral("invalid browser focus flag"));
    const auto source = object.value("sourceSize").toObject();
    double sourceWidth = 0, sourceHeight = 0;
    if (!finiteNumber(source, "width", sourceWidth, 1, 16384) ||
        !finiteNumber(source, "height", sourceHeight, 1, 16384) ||
        std::floor(sourceWidth) != sourceWidth || std::floor(sourceHeight) != sourceHeight)
        return reject(QStringLiteral("invalid source size"));
    report.sourceWidth = unsigned(sourceWidth); report.sourceHeight = unsigned(sourceHeight);
    report.media.width = report.sourceWidth; report.media.height = report.sourceHeight;
    if (object.contains("mediaInfo")) {
        if (!object.value("mediaInfo").isObject()) return reject(QStringLiteral("invalid media identity"));
        const auto media = object.value("mediaInfo").toObject();
        QString videoId; double epoch = 0;
        if (!identifier(media, "videoId", videoId) ||
            !finiteNumber(media, "sourceEpoch", epoch, 0, 9007199254740991.0) || std::floor(epoch) != epoch)
            return reject(QStringLiteral("invalid media identity"));
        report.media.hasIdentity = true; report.media.videoId = videoId.toStdString();
        report.media.sourceEpoch = quint64(epoch);
    }
    if (object.contains("frameRate") && !finiteNumber(object, "frameRate", report.frameRate, 0, 1000))
        return reject(QStringLiteral("invalid browser frame rate"));
    const auto holes = object.value("uiRects");
    if (!holes.isUndefined()) {
        if (!holes.isArray() || holes.toArray().size() > 96) return reject(QStringLiteral("invalid UI rectangles"));
        for (const auto value : holes.toArray()) {
            if (!value.isArray() || value.toArray().size() != 4) return reject(QStringLiteral("invalid UI rectangle"));
            std::array<double, 4> hole{}; const auto array = value.toArray();
            for (int i = 0; i < 4; ++i) {
                if (!array[i].isDouble()) return reject(QStringLiteral("invalid UI rectangle coordinate"));
                hole[size_t(i)] = array[i].toDouble();
                if (!std::isfinite(hole[size_t(i)]) || hole[size_t(i)] < 0 || hole[size_t(i)] > 1)
                    return reject(QStringLiteral("UI rectangle outside video"));
            }
            if (hole[2] <= hole[0] || hole[3] <= hole[1]) return reject(QStringLiteral("empty UI rectangle"));
            report.holes.push_back(hole);
        }
    }
    return true;
}
std::vector<RECT> holePixels(const Report& report, const Geometry& geometry) {
    std::vector<RECT> result; result.reserve(report.holes.size());
    for (const auto& hole : report.holes) result.push_back({
        LONG(std::floor(hole[0] * width(geometry.video))), LONG(std::floor(hole[1] * height(geometry.video))),
        LONG(std::ceil(hole[2] * width(geometry.video))), LONG(std::ceil(hole[3] * height(geometry.video)))});
    return result;
}
} // namespace

struct BrowserCaptureController::Impl {
    BrowserCaptureController* owner;
    engine::EngineController& engine;
    QTcpServer server;
    QTimer timer;
    VideoRegionOverlay overlay;
    engine::PlayerOptions options;
    engine::PresentationSettings presentation;
    std::optional<Report> report;
    // Retained authority only, never fed to capture. A fresh playing report
    // must match it and revalidate current geometry before any new engine open.
    std::optional<Report> pausedAuthority;
    std::optional<Geometry> activeGeometry, candidate;
    QSet<QTcpSocket*> sockets;
    QHash<QString, quint64> sequences;
    // Retain bindings after stop/expiry so old document reports cannot retarget
    // another browser window after the foreground changes.
    QHash<QString, BrowserBinding> bindings;
    // Epoch watermarks survive stop/expiry, independently of HTTP sequence.
    QHash<QString, quint64> mediaEpochs;
    qulonglong httpRequests = 0, httpRejected = 0, httpGetRequests = 0, httpPostRequests = 0;
    qulonglong httpStatusPostAccepted = 0, httpStatusGetAccepted = 0;
    qulonglong httpOriginMissingRejected = 0, httpOriginMismatchRejected = 0;
    bool httpLastOriginPresent = false, httpLastOriginMatches = false;
    bool httpLastRejectOriginPresent = false, httpLastRejectOriginMatches = false;
    QString httpLastMethod, httpLastPath, httpLastRejectMethod, httpLastRejectPath, httpLastRejectReason;
    int httpLastRejectCode = 0;
    bool enabled = false, opening = false, draining = false, configurationDirty = false;
    bool rtxSourceSized = true;
    std::function<bool(bool)> enableAdmission;
    QString status = QStringLiteral("已关闭；网页原画保持显示");
    QString reasonCode = QStringLiteral("disabled");
    qulonglong viewportRebindings = 0, sourceResets = 0;
    ULONGLONG stableSince = 0, openedAt = 0, publishedAt = 0, nextLog = 0, rateWindow = 0;
    int messagesThisWindow = 0;
    uint64_t engineSession = 0;
    unsigned mismatchCount = 0;
    BrowserTelemetrySource telemetrySource;
    BrowserTelemetryCache telemetryCache;
    HWND monitorWindow = nullptr;
    Impl(BrowserCaptureController* q, engine::EngineController& e) : owner(q), engine(e) {
        options = engine::PlayerOptions::from(BrowserCaptureController::defaultSettings());
        presentation.enabled = true; presentation.outputRate = engine::OutputRateMode::FollowDisplay;
        presentation.display = engine::DisplaySync::Automatic;
        server.setMaxPendingConnections(kConnectionLimit);
        QObject::connect(&server, &QTcpServer::newConnection, owner, [this] { acceptConnections(); });
        timer.setInterval(40);
        QObject::connect(&timer, &QTimer::timeout, owner, [this] { tick(); });
        timer.start();
    }
    void publish(bool force = false) {
        const auto now = GetTickCount64();
        if (force || now - publishedAt >= 200) {
            telemetryCache.update(telemetrySource, activeGeometry ? engineSession : 0,
                overlay.visible(), engine.snapshot(), now, !configurationDirty);
            publishedAt = now; emit owner->changed();
        }
    }
    void stopCapture(const QString& reason, const QString& code = QStringLiteral("waiting-video")) {
        if (code != QLatin1String("paused")) pausedAuthority.reset();
        overlay.hide();
        if (activeGeometry || opening) { engine.stop(); draining = true; }
        activeGeometry.reset(); candidate.reset(); opening = false; engineSession = 0;
        status = reason; reasonCode = code;
    }
    void fail(const QString& reason) {
        const bool changed = enabled; enabled = false; report.reset();
        stopCapture(reason, QStringLiteral("engine-failed"));
        log::error("browser-attachment", reason.toStdString());
        if (changed) emit owner->enabledChanged(false);
        publish(true);
    }
    engine::PlayerOptions effectiveOptions(const Geometry& geometry) const {
        auto effective = options;
        effective.settings = engine::displayPlaybackSettings(options.snapshot(), geometry.display, true, true);
        effective.settings.videoSrInputExtent = {};
        if (report && effective.sr) {
            effective.settings.videoSrInputExtent = engine::browserSrInputExtent(
                {unsigned(width(geometry.video)), unsigned(height(geometry.video))},
                {report->sourceWidth, report->sourceHeight},effective.settings.videoSrQuality,
                rtxSourceSized,GetEnvironmentVariableW(L"VEYRA_BROWSER_RTX_FULL_CAPTURE", nullptr, 0)!=0);
        }
        return engine::PlayerOptions::from(effective.settings, options.nodeOrder);
    }
    bool openCapture(const Geometry& geometry) {
        if (!engine.idle()) return false;
        overlay.destroy();
        if (!overlay.create(geometry.browser, geometry.video) ||
            !overlay.setExcludedRects(holePixels(*report, geometry))) {
            fail(QStringLiteral("视频覆盖窗口创建失败，已恢复原画；请手动重新开启")); return false;
        }
        source::ScreenCaptureOptions capture;
        capture.kind = source::ScreenTargetKind::Window; capture.method = source::ScreenCaptureMethod::Wgc;
        capture.target = reinterpret_cast<uint64_t>(geometry.browser); capture.cursor = false; capture.fps = 0;
        capture.left = unsigned(geometry.video.left - geometry.frame.left);
        capture.top = unsigned(geometry.video.top - geometry.frame.top);
        capture.right = unsigned(geometry.frame.right - geometry.video.right);
        capture.bottom = unsigned(geometry.frame.bottom - geometry.video.bottom);
        auto effective = effectiveOptions(geometry);
        engine.requestPresentation(presentation); engine.previewView({});
        engine.open(overlay.handle(), capture.uri(), effective);
        engineSession = engine.snapshot().sessionId;
        activeGeometry = geometry; candidate.reset(); opening = true; draining = false;
        openedAt = GetTickCount64(); configurationDirty = false;
        status = QStringLiteral("正在捕获视频区域并启动增强；网页布局保持原样");
        reasonCode = QStringLiteral("starting");
        log::info("browser-attachment", std::format(
            "open browser={} viewport={} output={} rect={},{},{}x{} frame={}x{} crop={},{},{},{} monitor={} dpi={} display={}x{} srTarget={}x{} nr={} sr={} fg={} inputClock=compositor audioCapture=0",
            uint64_t(geometry.browser), uint64_t(geometry.viewport), uint64_t(overlay.handle()),
            geometry.video.left, geometry.video.top, width(geometry.video), height(geometry.video),
            width(geometry.frame), height(geometry.frame), capture.left, capture.top, capture.right, capture.bottom,
            uint64_t(geometry.monitor), geometry.dpi, geometry.display.width, geometry.display.height,
            effective.settings.srPlaybackExtent.width, effective.settings.srPlaybackExtent.height,
            effective.nr, effective.sr, effective.fg));
        publish(true); return true;
    }
    void tick() {
        const BrowserCallbackTimingProbe callbackTiming("tick");
        if (draining && engine.idle()) { overlay.destroy(); draining = false; }
        if (!enabled) { publish(); return; }
        const auto now = GetTickCount64();
        if (!report) {
            if (pausedAuthority) {
                const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
                if (foreground != pausedAuthority->binding.browser && !VideoRegionOverlay::knownMonitorForeground(monitorWindow, overlay.handle()))
                    pausedAuthority.reset();
            }
            // Keep a specific fallback/pause reason readable until a new report,
            // rather than immediately erasing it on the next 40ms tick.
            if (activeGeometry || opening || candidate)
                stopCapture(QStringLiteral("等待当前浏览器视频报告；网页原画保持显示"));
            publish(); return;
        }
        if (now - report->receivedAt > kReportExpiryMs || !report->playing) {
            const bool paused = !report->playing;
            stopCapture(paused ? QStringLiteral("视频已暂停；显示网页原画") :
                QStringLiteral("浏览器视频报告已过期；网页原画保持显示"),
                paused ? QStringLiteral("paused") : QStringLiteral("report-expired"));
            if (!paused) report.reset();
            publish(); return;
        }
        QString reason, geometryReasonCode;
        auto geometry = geometryFor(*report, reason, geometryReasonCode, false, monitorWindow, overlay.handle(), &*report);
        if (!geometry) { stopCapture(reason, geometryReasonCode); publish(); return; }
        if (activeGeometry && sameCapture(*activeGeometry, *geometry)) {
            if (!overlay.updateGeometry(geometry->browser, geometry->video) ||
                !overlay.setExcludedRects(holePixels(*report, *geometry))) {
                fail(QStringLiteral("视频覆盖位置更新失败，已恢复原画；请手动重新开启")); return;
            }
            activeGeometry = geometry;
            telemetrySource.displayWidth = geometry->display.width;
            telemetrySource.displayHeight = geometry->display.height;
            telemetrySource.videoWidth = unsigned(width(geometry->video));
            telemetrySource.videoHeight = unsigned(height(geometry->video));
            // Report media changes already stop/reopen capture. Only explicit
            // configuration changes submit here: an engine rollback must not
            // cause a 40ms loop of the same rejected graph initialization.
            if (configurationDirty) {
                auto effective = effectiveOptions(*geometry);
                if (!engine.requestSettings(effective.settings, effective.nodeOrder)) {
                    fail(QStringLiteral("浏览器增强设置被引擎拒绝，已恢复原画；请检查方案")); return;
                }
                engine.requestPresentation(presentation); configurationDirty = false;
            }
            const auto snapshot = engine.snapshot();
            if (snapshot.sessionId != engineSession) {
                fail(QStringLiteral("播放来源已切换，浏览器增强已关闭")); return;
            }
            if (snapshot.failed) {
                fail(QStringLiteral("浏览器增强失败，已恢复原画：") + QString::fromStdWString(snapshot.status)); return;
            }
            if (snapshot.sourceWidth && (snapshot.sourceWidth != unsigned(width(geometry->video)) ||
                snapshot.sourceHeight != unsigned(height(geometry->video)))) {
                log::error("browser-attachment", std::format("crop mismatch expected={}x{} actual={}x{}",
                    width(geometry->video), height(geometry->video), snapshot.sourceWidth, snapshot.sourceHeight));
                if (++mismatchCount >= 3) fail(QStringLiteral("捕获尺寸连续不匹配，已恢复原画；请手动重新开启"));
                else stopCapture(QStringLiteral("捕获尺寸已变化，等待重新匹配；显示原画"));
                publish(); return;
            }
            if (snapshot.running && snapshot.sourceWidth == unsigned(width(geometry->video)) &&
                snapshot.sourceHeight == unsigned(height(geometry->video)) &&
                snapshot.metrics.flow.counters.realPresented > 0) {
                if (!overlay.showAfterPresent()) {
                    if (overlay.lastError() == ERROR_NOT_READY || overlay.lastError() == ERROR_NOT_SUPPORTED)
                        stopCapture(QStringLiteral("当前窗口状态不支持覆盖；显示网页原画"));
                    else fail(QStringLiteral("视频覆盖显示失败，已恢复原画；请手动重新开启"));
                    publish(); return;
                }
                opening = false; mismatchCount = 0;
                status = QStringLiteral("视频区域增强运行中；输入时钟为桌面合成器，声音由浏览器播放");
                reasonCode = QStringLiteral("running");
            } else if (policy::firstPresentDeadlineExpired(opening, now - openedAt)) {
                fail(QStringLiteral("等待增强首帧超时，已恢复原画；请手动重新开启")); return;
            }
            if (now >= nextLog) {
                const auto& flow = snapshot.metrics.flow;
                log::info("browser-attachment-stats", std::format(
                    "session={} visible={} source={}x{} vsrInput={}x{} browserSource={}x{} browserSampleFps={:.3f} nr={} sr={} fg={} realPresented={} generatedPresented={} xessSdkGenerated={} submitFps={:.3f} (not physical display FPS)",
                    engineSession, overlay.visible(), snapshot.sourceWidth, snapshot.sourceHeight,
                    snapshot.videoSrInputExtent.width, snapshot.videoSrInputExtent.height,
                    report->sourceWidth, report->sourceHeight, report->frameRate, snapshot.nrActive,
                    snapshot.srActive, snapshot.fgActive, flow.counters.realPresented,
                    flow.counters.generatedPresented, flow.counters.xessSdkGenerated, flow.presentSubmitFps));
                nextLog = now + 1000;
            }
            publish(); return;
        }
        if (activeGeometry || opening)
            stopCapture(QStringLiteral("视频尺寸或屏幕变化，等待稳定后恢复增强"), QStringLiteral("source-changed"));
        if (!engine.idle()) { publish(); return; }
        if (!candidate || !sameCapture(*candidate, *geometry) || !sameRect(candidate->video, geometry->video)) {
            candidate = geometry; stableSince = now; publish(); return;
        }
        if (now - stableSince >= kStableMs) openCapture(*geometry);
        publish();
    }
    QVariantMap diagnostics() const {
        const BrowserCallbackTimingProbe callbackTiming("diagnostics");
        const auto snapshot = engine.snapshot(); const auto& flow = snapshot.metrics.flow;
        const bool ownSession = activeGeometry && snapshot.sessionId == engineSession;
        QString monitorReason, monitorCode;
        const Report* authority = report ? &*report : pausedAuthority ? &*pausedAuthority : nullptr;
        const bool authorityFresh = pausedAuthority || (report && report->playing &&
            GetTickCount64() - report->receivedAt <= kReportExpiryMs);
        const bool ownMonitorForeground = enabled && authority && authorityFresh && monitorWindow &&
            VideoRegionOverlay::knownMonitorForeground(monitorWindow, overlay.handle()) &&
            geometryFor(*authority, monitorReason, monitorCode, false, monitorWindow, overlay.handle(), authority).has_value();
        QVariantMap result{{"schema", 1}, {"enabled", enabled}, {"status", status}, {"reasonCode", reasonCode},
            {"state", enabled ? (overlay.visible() ? "running" : "waiting") : "stopped"},
            {"listenerRunning", server.isListening()}, {"port", int(kPort)},
            {"captureActive", bool(activeGeometry)}, {"overlayVisible", overlay.visible()},
            {"srActive", ownSession && snapshot.srActive}, {"fgActive", ownSession && snapshot.fgActive},
            {"nrActive", ownSession && snapshot.nrActive}, {"sourceWidth", ownSession ? int(snapshot.sourceWidth) : 0},
            {"sourceHeight", ownSession ? int(snapshot.sourceHeight) : 0},
            {"realPresented", QVariant::fromValue<qulonglong>(ownSession ? flow.counters.realPresented : 0)},
            {"generatedPresented", QVariant::fromValue<qulonglong>(ownSession ? flow.counters.generatedPresented : 0)},
            {"xessSdkGenerated", QVariant::fromValue<qulonglong>(ownSession ? flow.counters.xessSdkGenerated : 0)},
            {"xessSdkPresented", QVariant::fromValue<qulonglong>(ownSession ? flow.counters.xessSdkPresented : 0)},
            {"presentSubmitFps", ownSession ? flow.presentSubmitFps : 0},
            {"browserFrameRate", report ? report->frameRate : 0},
            {"rtxSourceSized", rtxSourceSized},
            {"videoSrInputWidth", ownSession && snapshot.srActive ? int(snapshot.videoSrInputExtent.width) : 0},
            {"videoSrInputHeight", ownSession && snapshot.srActive ? int(snapshot.videoSrInputExtent.height) : 0},
            {"videoSrInputResampled", ownSession && snapshot.srActive && snapshot.videoSrInputExtent.valid() &&
                snapshot.videoSrInputExtent != pipeline::Extent{snapshot.sourceWidth, snapshot.sourceHeight}},
            {"browserSourceWidth", report ? int(report->sourceWidth) : 0},
            {"browserSourceHeight", report ? int(report->sourceHeight) : 0},
            {"sourceEpoch", QVariant::fromValue<qulonglong>(report && report->media.hasIdentity ? report->media.sourceEpoch : 0)},
            {"mediaIdentityAvailable", report && report->media.hasIdentity},
            {"viewportRebindings", QVariant::fromValue(viewportRebindings)},
            {"sourceResets", QVariant::fromValue(sourceResets)},
            {"inputClock", "compositor"}, {"audioCapture", false}, {"submissionIsDisplayFps", false}};
        // Native evidence, scoped to the last validated bound document/media.
        // The extension cannot grant this foreground exception in a request.
        result["ownMonitorForeground"] = ownMonitorForeground;
        result["boundBrowserWindowId"] = authority ? authority->windowInfo.value("browserWindowId").toInt(-1) : -1;
        result["boundTabId"] = authority ? authority->windowInfo.value("tabId").toInt(-1) : -1;
        result["boundDocumentId"] = authority ? authority->windowInfo.value("documentId").toString() : QString{};
        result["boundVideoId"] = authority ? QString::fromStdString(authority->media.videoId) : QString{};
        result["boundSourceEpoch"] = QVariant::fromValue<qulonglong>(authority ? authority->media.sourceEpoch : 0);
        result["http"] = QVariantMap{
            {"requests", QVariant::fromValue(httpRequests)}, {"rejected", QVariant::fromValue(httpRejected)},
            {"getRequests", QVariant::fromValue(httpGetRequests)}, {"postRequests", QVariant::fromValue(httpPostRequests)},
            {"statusPostAccepted", QVariant::fromValue(httpStatusPostAccepted)},
            {"statusGetAccepted", QVariant::fromValue(httpStatusGetAccepted)},
            {"originMissingRejected", QVariant::fromValue(httpOriginMissingRejected)},
            {"originMismatchRejected", QVariant::fromValue(httpOriginMismatchRejected)},
            {"latestMethod", httpLastMethod}, {"latestPath", httpLastPath},
            {"latestOriginPresent", httpLastOriginPresent}, {"latestOriginMatches", httpLastOriginMatches},
            {"lastRejectMethod", httpLastRejectMethod}, {"lastRejectPath", httpLastRejectPath},
            {"lastRejectOriginPresent", httpLastRejectOriginPresent},
            {"lastRejectOriginMatches", httpLastRejectOriginMatches},
            {"lastRejectCode", httpLastRejectCode}, {"lastRejectReason", httpLastRejectReason}};
        if (activeGeometry) {
            static constexpr const char* modes[]{"unavailable", "win32-physical", "visible-physical", "win32-scaled", "visible-scaled"};
            result["outerBoundsConvention"] = modes[int(activeGeometry->outerMatch)];
            result["displayWidth"] = int(activeGeometry->display.width); result["displayHeight"] = int(activeGeometry->display.height);
            result["videoLeft"] = int(activeGeometry->video.left); result["videoTop"] = int(activeGeometry->video.top);
            result["videoWidth"] = int(width(activeGeometry->video)); result["videoHeight"] = int(height(activeGeometry->video));
            result["targetWidth"] = int(snapshot.desired.srPlaybackExtent.width);
            result["targetHeight"] = int(snapshot.desired.srPlaybackExtent.height);
        }
        return result;
    }
    void noteHttpRejection(QTcpSocket* socket, int code, const QString& reason) {
        ++httpRejected; httpLastRejectCode = code; httpLastRejectReason = reason;
        httpLastRejectMethod = socket->property("httpMethod").toString();
        httpLastRejectPath = socket->property("httpPath").toString();
        httpLastRejectOriginPresent = socket->property("httpOriginPresent").toBool();
        httpLastRejectOriginMatches = socket->property("httpOriginMatches").toBool();
        if (code == 403 && socket->property("httpOriginParsed").toBool()) {
            if (!httpLastRejectOriginPresent) { ++httpOriginMissingRejected; httpLastRejectReason = QStringLiteral("origin-missing"); }
            else if (!httpLastRejectOriginMatches) { ++httpOriginMismatchRejected; httpLastRejectReason = QStringLiteral("origin-mismatch"); }
        }
        publish();
    }
    void respond(QTcpSocket* socket, int code, const QString& origin, QJsonObject body = {}) {
        if (!sockets.contains(socket)) return;
        if (code >= 400) {
            const auto reason = body.value("error").toString();
            noteHttpRejection(socket, code, reason.isEmpty() ? QStringLiteral("http-%1-rejected").arg(code) : reason);
        }
        const auto bytes = code == 204 ? QByteArray{} : QJsonDocument(body).toJson(QJsonDocument::Compact);
        QByteArray response = "HTTP/1.1 " + QByteArray::number(code) + " Response\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: application/json\r\n";
        if (!origin.isEmpty()) response += "Access-Control-Allow-Origin: " + origin.toUtf8() +
            "\r\nVary: Origin\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nAccess-Control-Max-Age: 600\r\n";
        response += "Content-Length: " + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes;
        socket->setProperty("finished", true); socket->write(response); socket->disconnectFromHost();
    }
    void acceptConnections() {
        while (server.hasPendingConnections()) {
            QTcpSocket* socket = server.nextPendingConnection();
            if (!socket) break;
            if (sockets.size() >= kConnectionLimit || socket->peerAddress() != QHostAddress(QHostAddress::LocalHost)) {
                socket->abort(); socket->deleteLater(); continue;
            }
            sockets.insert(socket); socket->setReadBufferSize(kHeaderLimit + kBodyLimit + 1);
            auto* timeout = new QTimer(socket); timeout->setSingleShot(true); timeout->start(2000);
            QObject::connect(timeout, &QTimer::timeout, owner, [this, socket] {
                if (!socket->property("finished").toBool()) noteHttpRejection(socket, 0, QStringLiteral("request-timeout"));
                socket->abort();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, owner, [this, socket] {
                sockets.remove(socket); socket->deleteLater();
            });
            QObject::connect(socket, &QTcpSocket::readyRead, owner, [this, socket] { readRequest(socket); });
        }
    }
    void readRequest(QTcpSocket* socket) {
        if (socket->property("finished").toBool()) return;
        if (!socket->property("httpCounted").toBool()) {
            socket->setProperty("httpCounted", true); ++httpRequests;
            httpLastMethod.clear(); httpLastPath.clear();
            httpLastOriginPresent = false; httpLastOriginMatches = false;
        }
        QByteArray bytes = socket->property("request").toByteArray(); bytes += socket->readAll();
        if (bytes.size() > kHeaderLimit + kBodyLimit) { respond(socket, 413, {}, {{"error", "request too large"}}); return; }
        socket->setProperty("request", bytes);
        const qsizetype separator = bytes.indexOf("\r\n\r\n");
        if (separator < 0) {
            if (bytes.size() > kHeaderLimit) respond(socket, 413, {}, {{"error", "headers too large"}});
            return;
        }
        if (separator > kHeaderLimit) { respond(socket, 413, {}, {{"error", "headers too large"}}); return; }
        const auto lines = bytes.left(separator).split('\n');
        if (lines.isEmpty()) { respond(socket, 400, {}); return; }
        const auto first = lines.front().trimmed().split(' ');
        if (first.size() != 3 || (first[2] != "HTTP/1.1" && first[2] != "HTTP/1.0")) { respond(socket, 400, {}); return; }
        if (!socket->property("httpFirstParsed").toBool()) {
            socket->setProperty("httpFirstParsed", true);
            const auto method = first[0], path = first[1];
            httpLastMethod = (method == "GET" || method == "POST" || method == "OPTIONS") ? QString::fromLatin1(method) : QStringLiteral("other");
            httpLastPath = (path == "/v1/status" || path == "/v1/video" || path == "/v1/control") ? QString::fromLatin1(path) : QStringLiteral("other");
            socket->setProperty("httpMethod", httpLastMethod); socket->setProperty("httpPath", httpLastPath);
            if (method == "GET") ++httpGetRequests;
            else if (method == "POST") ++httpPostRequests;
        }
        QHash<QByteArray, QByteArray> headers;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines[i].trimmed(); const qsizetype colon = line.indexOf(':');
            if (colon < 1) { respond(socket, 400, {}); return; }
            const auto key = line.left(colon).trimmed().toLower(), value = line.mid(colon + 1).trimmed();
            if (headers.contains(key)) { respond(socket, 400, {}); return; }
            headers.insert(key, value);
        }
        const QString origin = QString::fromLatin1(headers.value("origin"));
        static const QRegularExpression extensionOrigin(QStringLiteral("^chrome-extension://[a-p]{32}$"));
        const bool originPresent = headers.contains("origin"), originMatches = extensionOrigin.match(origin).hasMatch() &&
            origin == QLatin1String("chrome-extension://ahiondbmdpeodpldmaclilcggpcdaleg");
        socket->setProperty("httpOriginParsed", true); socket->setProperty("httpOriginPresent", originPresent);
        socket->setProperty("httpOriginMatches", originMatches);
        httpLastOriginPresent = originPresent; httpLastOriginMatches = originMatches;
        if (!originMatches) {
            respond(socket, 403, {}, {{"error", "Veyra extension origin required"}}); return;
        }
        if (headers.value("host") != "127.0.0.1:24861" || headers.contains("transfer-encoding")) {
            respond(socket, 400, origin, {{"error", "invalid host or transfer encoding"}}); return;
        }
        const QByteArray method = first[0], path = first[1];
        if (path != "/v1/status" && path != "/v1/video" && path != "/v1/control") {
            respond(socket, 404, origin); return;
        }
        qint64 length = 0;
        if (headers.contains("content-length")) {
            const QByteArray text = headers.value("content-length");
            if (text.isEmpty() || std::any_of(text.begin(), text.end(), [](char c) { return c < '0' || c > '9'; })) {
                respond(socket, 400, origin); return;
            }
            bool valid = false; length = text.toLongLong(&valid);
            if (!valid || length < 0 || length > kBodyLimit) { respond(socket, 413, origin); return; }
        }
        if (method == "OPTIONS") {
            const auto requested = headers.value("access-control-request-method");
            const auto requestedHeaders = headers.value("access-control-request-headers").toLower();
            if (length || (requested != "POST" && requested != "GET") ||
                (!requestedHeaders.isEmpty() && requestedHeaders != "content-type")) respond(socket, 403, origin);
            else respond(socket, 204, origin);
            return;
        }
        if ((method == "GET" && path != "/v1/status") ||
            (method != "GET" && method != "POST")) {
            respond(socket, 405, origin); return;
        }
        if (method == "GET" && length) { respond(socket, 400, origin); return; }
        if (method == "POST" && (!headers.contains("content-length") || length == 0 ||
            headers.value("content-type").toLower() != "application/json")) {
            respond(socket, 415, origin, {{"error", "application/json required"}}); return;
        }
        const qsizetype total = separator + 4 + length;
        if (bytes.size() < total) return;
        if (bytes.size() != total) { respond(socket, 400, origin, {{"error", "one request per connection"}}); return; }
        const auto now = GetTickCount64();
        if (now - rateWindow >= 1000) { rateWindow = now; messagesThisWindow = 0; }
        if (++messagesThisWindow > kMessagesPerSecond) { respond(socket, 429, origin); return; }
        if (method == "GET") {
            ++httpStatusGetAccepted;
            respond(socket, 200, origin, QJsonObject::fromVariantMap(diagnostics())); return;
        }
        QJsonParseError parseError; const auto document = QJsonDocument::fromJson(bytes.mid(separator + 4, length), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) { respond(socket, 400, origin); return; }
        const auto object = document.object();
        if (path == "/v1/status") {
            if (object.size() != 1 || !object.value("schema").isDouble() || object.value("schema").toDouble() != 1) {
                respond(socket, 400, origin, {{"error", "status schema required"}}); return;
            }
            ++httpStatusPostAccepted;
            respond(socket, 200, origin, QJsonObject::fromVariantMap(diagnostics())); return;
        }
        if (path == "/v1/control") {
            if (!object.value("schema").isDouble() || object.value("schema").toDouble() != 1 ||
                !object.value("enabled").isBool()) { respond(socket, 400, origin); return; }
            owner->setEnabledFromUser(object.value("enabled").toBool());
            respond(socket, 200, origin, QJsonObject::fromVariantMap(diagnostics())); return;
        }
        Report incoming; bool stop = false; QString error;
        if (!parseReport(object, origin, incoming, stop, error)) {
            // A transform/shape change from the active document cannot leave an
            // old rectangle on screen. Invalid foreign reports have no authority.
            if (report && report->key() == incoming.key() && incoming.sequence > report->sequence &&
                incoming.sequence > sequences.value(incoming.key(), 0)) {
                sequences[incoming.key()] = incoming.sequence;
                report.reset(); stopCapture(QStringLiteral("网页视频坐标不可用；显示原画"),
                    QStringLiteral("invalid-report")); publish(true);
            }
            respond(socket, 400, origin, {{"error", error}}); return;
        }
        const QString key = incoming.key();
        if (sequences.contains(key) && incoming.sequence <= sequences.value(key)) {
            respond(socket, 409, origin, {{"error", "stale sequence"}}); return;
        }
        if (sequences.size() >= 128 && !sequences.contains(key)) {
            respond(socket, 429, origin, {{"error", "too many sessions"}}); return;
        }
        sequences[key] = incoming.sequence;
        const QString bindingKey = incoming.bindingKey();
        QString mediaKey;
        if (!stop && incoming.media.hasIdentity) {
            mediaKey = bindingKey + QChar(0x1d) +
                QString::fromLatin1(QByteArray::fromStdString(incoming.media.videoId).toHex());
            // Paused video metadata is still media-scoped. A newer HTTP
            // sequence cannot let an old epoch stop the current source.
            if (mediaEpochs.contains(mediaKey) && incoming.media.sourceEpoch < mediaEpochs.value(mediaKey)) {
                auto response = QJsonObject::fromVariantMap(diagnostics());
                response["error"] = "stale source epoch"; response["accepted"] = false;
                response["reasonCode"] = "stale-source-epoch";
                respond(socket, 409, origin, response); return;
            }
        }
        if (stop || !incoming.playing) {
            if (report && report->key() == key) {
                const bool samePausedMedia = stop || (report->documentKey() == incoming.documentKey() &&
                    policy::sameMedia(report->media, incoming.media));
                const bool pause = samePausedMedia && (!stop || incoming.stopReason == QLatin1String("video-paused") ||
                    incoming.stopReason == QLatin1String("video-ended"));
                if (!samePausedMedia) {
                    telemetryCache.clearSource(); telemetrySource = {};
                }
                if (pause) { pausedAuthority = *report; pausedAuthority->playing = false; }
                report.reset(); stopCapture(QStringLiteral("当前网页视频已停止；显示原画"),
                    pause ? QStringLiteral("paused") : QStringLiteral("stopped")); publish(true);
            } else if (pausedAuthority && pausedAuthority->key() == key) {
                const bool samePausedMedia = stop ? incoming.stopReason == QLatin1String("video-paused") ||
                    incoming.stopReason == QLatin1String("video-ended") :
                    pausedAuthority->documentKey() == incoming.documentKey() && policy::sameMedia(pausedAuthority->media, incoming.media);
                if (!samePausedMedia) { pausedAuthority.reset(); publish(true); }
            }
            respond(socket, 200, origin, QJsonObject::fromVariantMap(diagnostics())); return;
        }
        if (!enabled) { respond(socket, 200, origin, QJsonObject::fromVariantMap(diagnostics())); return; }
        if (bindings.contains(bindingKey)) incoming.binding = bindings.value(bindingKey);
        else if (bindings.size() >= 128) {
            respond(socket, 429, origin, {{"error", "too many document bindings"}}); return;
        }
        if (!mediaKey.isEmpty()) {
            if (mediaEpochs.size() >= 512 && !mediaEpochs.contains(mediaKey)) {
                respond(socket, 429, origin, {{"error", "too many media identities"}}); return;
            }
        }
        QString reason, geometryReasonCode;
        const Report* authority = report ? &*report : pausedAuthority ? &*pausedAuthority : nullptr;
        const auto matched = geometryFor(incoming, reason, geometryReasonCode, true, monitorWindow, overlay.handle(), authority);
        if (!matched) {
            if (report && report->origin == origin && report->client == incoming.client) {
                report.reset(); stopCapture(reason, geometryReasonCode); publish(true);
            } else if (!activeGeometry) { status = reason; reasonCode = geometryReasonCode; publish(true); }
            auto response = QJsonObject::fromVariantMap(diagnostics()); response["accepted"] = false;
            respond(socket, 200, origin, response); return;
        }
        const bool viewportRebound = incoming.binding.viewport && incoming.binding.viewport != matched->viewport;
        const bool sourceChanged = report && (report->key() != key ||
            report->documentKey() != incoming.documentKey() || !policy::sameMedia(report->media, incoming.media));
        if (viewportRebound || sourceChanged) {
            if (viewportRebound) ++viewportRebindings;
            if (sourceChanged) ++sourceResets;
            // Hide synchronously before installing new metadata. Existing
            // stop/open resets all engine history after the stable debounce.
            stopCapture(QStringLiteral("网页来源或视口变化，等待稳定后恢复增强"),
                viewportRebound ? QStringLiteral("viewport-changed") : QStringLiteral("source-changed"));
        }
        incoming.binding = {matched->browser, matched->viewport, matched->processId, matched->processCreatedAt};
        bindings.insert(bindingKey, incoming.binding);
        if (!mediaKey.isEmpty()) mediaEpochs.insert(mediaKey, incoming.media.sourceEpoch);
        incoming.receivedAt = now; report = std::move(incoming);
        pausedAuthority.reset();
        telemetrySource.available = true;
        telemetrySource.document = report->bindingKey().toStdString();
        telemetrySource.browser = report->binding.identity();
        telemetrySource.media = report->media;
        telemetrySource.originalWidth = report->sourceWidth;
        telemetrySource.originalHeight = report->sourceHeight;
        telemetrySource.originalFps = report->frameRate;
        telemetrySource.displayWidth = matched->display.width;
        telemetrySource.displayHeight = matched->display.height;
        telemetrySource.videoWidth = unsigned(width(matched->video));
        telemetrySource.videoHeight = unsigned(height(matched->video));
        tick(); auto response = QJsonObject::fromVariantMap(diagnostics()); response["accepted"] = true;
        respond(socket, 200, origin, response);
    }
};

engine::EnhancementSettings BrowserCaptureController::defaultSettings() {
    engine::EnhancementSettings settings;
    for (const auto& preset : engine::nativePlaybackPresets()) {
        if (preset.id == "smooth-4k") { engine::PresetLibrary::apply(preset.entry, settings); break; }
    }
    settings.nr = false; settings.sr = true; settings.videoSrQuality = 4;
    settings.multiplier = 2; settings.frameGenerationBackend = engine::FrameGenerationBackend::XeSS;
    settings.srMotion = engine::MotionSource::Zero; settings.fgMotion = engine::MotionSource::OpticalFlow;
    settings.srPlaybackExtent = {}; settings.videoSrInputExtent = {}; return settings;
}
BrowserCaptureController::BrowserCaptureController(engine::EngineController& engine, QObject* parent)
    : QObject(parent), p_(std::make_unique<Impl>(this, engine)) {}
BrowserCaptureController::~BrowserCaptureController() {
    p_->timer.stop(); p_->server.close();
    for (auto* socket : p_->sockets) { socket->disconnect(this); socket->abort(); }
    p_->enabled = false; p_->stopCapture(QStringLiteral("应用退出"));
    // Teardown only: the output HWND must outlive its worker/swapchain. Normal
    // ticks never wait for GPU work, and no QObject event pumping is required.
    while (p_->draining && !p_->engine.idle()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    p_->overlay.destroy();
}
bool BrowserCaptureController::startListening() {
    if (p_->server.isListening()) return true;
    if (!p_->server.listen(QHostAddress::LocalHost, kPort)) {
        p_->status = QStringLiteral("浏览器接收端口不可用：") + p_->server.errorString();
        p_->reasonCode = QStringLiteral("listener-unavailable");
        log::error("browser-endpoint", p_->status.toStdString()); p_->publish(true); return false;
    }
    log::info("browser-endpoint", "listening=127.0.0.1:24861 source=extension-origin metadataNeverEnables=1");
    p_->publish(true); return true;
}
bool BrowserCaptureController::listenerRunning() const { return p_->server.isListening(); }
void BrowserCaptureController::setEnableAdmission(std::function<bool(bool)> admission) {
    p_->enableAdmission = std::move(admission);
}
void BrowserCaptureController::setEnabled(bool enabled) {
    if (p_->enabled == enabled) return;
    if (p_->enableAdmission && !p_->enableAdmission(enabled)) {
        p_->status = QStringLiteral("独立片源配置不可用，保持当前播放");
        p_->reasonCode = QStringLiteral("profile-unavailable");
        log::warn("source-profiles", "browser transition refused before stopping source");
        p_->publish(true); return;
    }
    p_->enabled = enabled; p_->report.reset(); p_->mismatchCount = 0;
    p_->stopCapture(enabled ? QStringLiteral("已开启，等待前台浏览器主视频") : QStringLiteral("已关闭；网页原画保持显示"),
        enabled ? QStringLiteral("waiting-video") : QStringLiteral("disabled"));
    if (enabled && !p_->engine.idle()) p_->engine.stop();
    log::info("browser-attachment", std::format("browserEnabled={}", enabled));
    emit enabledChanged(enabled); p_->publish(true);
}
void BrowserCaptureController::setEnabledFromUser(bool enabled) {
    setEnabled(enabled);
    // A rejected transition must not persist an ON/OFF preference that never
    // took effect. Repeated successful OFF still records the explicit choice.
    if (p_->enabled == enabled) emit userEnabledRequested(enabled);
}
bool BrowserCaptureController::enabled() const { return p_->enabled; }
bool BrowserCaptureController::active() const { return bool(p_->activeGeometry) || p_->opening || p_->draining; }
QString BrowserCaptureController::status() const { return p_->status; }
QVariantMap BrowserCaptureController::diagnostics() const { return p_->diagnostics(); }
BrowserTelemetryView BrowserCaptureController::telemetry() const { return p_->telemetryCache.view(); }
uint64_t BrowserCaptureController::previewSessionId() const {
    return p_->enabled && p_->activeGeometry ? p_->engineSession : 0;
}
void BrowserCaptureController::setRtxSourceSized(bool enabled) {
    if (p_->rtxSourceSized == enabled) return;
    p_->rtxSourceSized = enabled;
    p_->configurationDirty = true;
    p_->telemetryCache.clearSample();
    log::info("browser-rtx", std::format("sourceSized={} input=compositor-resample qualityLevelPreserved=true", enabled));
    p_->publish(true);
}
HWND BrowserCaptureController::sourceWindow() const {
    return p_->activeGeometry ? p_->activeGeometry->browser : nullptr;
}
void BrowserCaptureController::setMonitorWindow(HWND window) {
    DWORD processId = 0;
    if (window) GetWindowThreadProcessId(window, &processId);
    p_->monitorWindow = window && IsWindow(window) && GetAncestor(window, GA_ROOT) == window &&
        processId == GetCurrentProcessId() ? window : nullptr;
    p_->overlay.setMonitorWindow(p_->monitorWindow);
    p_->publish(true);
}
void BrowserCaptureController::configure(engine::EnhancementSettings settings, engine::PresentationSettings presentation) {
    configure(engine::PlayerOptions::from(std::move(settings)), presentation);
}
void BrowserCaptureController::configure(engine::PlayerOptions options, engine::PresentationSettings presentation) {
    if (!options.snapshot().validate().empty() || !presentation.valid()) {
        p_->status = QStringLiteral("浏览器增强设置无效，保留已有方案");
        p_->reasonCode = QStringLiteral("invalid-settings"); p_->publish(true); return;
    }
    options.settings.srPlaybackExtent = {};
    options.settings.videoSrInputExtent = {};
    presentation.fullscreen = false;
    if (options.snapshot() != p_->options.snapshot() || options.nodeOrder != p_->options.nodeOrder)
        p_->telemetryCache.clearSample();
    p_->options = std::move(options); p_->presentation = presentation; p_->configurationDirty = true;
    p_->publish(true);
}
} // namespace veyra::capture
