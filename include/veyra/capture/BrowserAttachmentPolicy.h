#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <utility>

namespace veyra::capture::policy {
// Pure attachment decisions. No window operations or GPU work occur here.
inline constexpr bool firstPresentDeadlineExpired(bool opening, std::uint64_t elapsedMs) {
    // Presentation counters can restart after a metrics/temporal reset. Only
    // a capture still awaiting its first successful present has a startup deadline.
    return opening && elapsedMs > 15000;
}
struct BrowserIdentity {
    std::uintptr_t window = 0;
    std::uint32_t processId = 0;
    std::uint64_t processCreatedAt = 0;
    bool valid() const { return window && processId && processCreatedAt; }
};
inline bool sameBrowser(const BrowserIdentity& a, const BrowserIdentity& b) {
    return a.valid() && b.valid() && a.window == b.window &&
        a.processId == b.processId && a.processCreatedAt == b.processCreatedAt;
}
enum class ForegroundAuthority { Reject, Browser, OwnVeyraMonitor };
struct ForegroundEvidence {
    bool browserForeground = false, ownMonitorForeground = false;
    bool boundBrowserValid = false, browserVisible = false, browserMinimized = false;
    bool regionVisible = false;
};
inline constexpr ForegroundAuthority foregroundAuthority(const ForegroundEvidence& evidence) {
    if (!evidence.browserVisible || evidence.browserMinimized || !evidence.regionVisible)
        return ForegroundAuthority::Reject;
    if (evidence.browserForeground) return ForegroundAuthority::Browser;
    if (evidence.ownMonitorForeground && evidence.boundBrowserValid)
        return ForegroundAuthority::OwnVeyraMonitor;
    return ForegroundAuthority::Reject;
}
enum class OverlayPlacement { Reject, AboveSource, TopOfOrdinaryGroup };
inline constexpr OverlayPlacement overlayPlacement(bool browserForeground, bool hasPredecessor,
                                                   bool predecessorTopmost) {
    // Inserting after a topmost HWND would promote the overlay. When that is
    // the source's immediate predecessor, the source leads the ordinary group.
    if (hasPredecessor && !predecessorTopmost) return OverlayPlacement::AboveSource;
    if (hasPredecessor || browserForeground) return OverlayPlacement::TopOfOrdinaryGroup;
    return OverlayPlacement::Reject;
}
// Hit owners are already checked against the exact browser root by the native
// adapter. Zero also represents an unknown or ambiguously nested hit.
inline std::uintptr_t selectViewportCandidate(const std::vector<std::uintptr_t>& candidates,
                                              const std::vector<std::uintptr_t>& hitOwners) {
    if (candidates.empty()) return 0;
    for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        if (!*it || std::find(candidates.begin(), it, *it) != it) return 0;
    }
    // Keep the existing single-candidate path independent of hit testing.
    if (candidates.size() == 1) return candidates.front();
    if (hitOwners.size() < 3 || !hitOwners.front() ||
        std::find(candidates.begin(), candidates.end(), hitOwners.front()) == candidates.end()) return 0;
    return std::all_of(hitOwners.begin(), hitOwners.end(), [&](std::uintptr_t owner) {
        return owner == hitOwners.front();
    }) ? hitOwners.front() : 0;
}

enum class ViewportDecision { Keep, BindFirst, ReplaceDestroyed, RejectBrowser, RejectViewport };
inline ViewportDecision viewportDecision(const BrowserIdentity& bound, const BrowserIdentity& observed,
                                        std::uintptr_t oldViewport, std::uintptr_t newViewport,
                                        bool oldViewportAlive, bool freshMetadata) {
    if (!observed.valid() || (bound.window && !sameBrowser(bound, observed)))
        return ViewportDecision::RejectBrowser;
    if (!newViewport) return ViewportDecision::RejectViewport;
    if (!oldViewport) return ViewportDecision::BindFirst;
    if (oldViewport == newViewport) return ViewportDecision::Keep;
    // Hidden/reparented live widgets may belong to another tab. Only a fresh
    // report in the original browser can replace a genuinely destroyed HWND.
    if (freshMetadata && !oldViewportAlive && sameBrowser(bound, observed))
        return ViewportDecision::ReplaceDestroyed;
    return ViewportDecision::RejectViewport;
}
struct MediaFingerprint {
    std::string videoId;
    std::uint64_t sourceEpoch = 0;
    unsigned width = 0, height = 0;
    bool hasIdentity = false;
};
inline bool sameMedia(const MediaFingerprint& a, const MediaFingerprint& b) {
    return a.hasIdentity == b.hasIdentity && a.videoId == b.videoId &&
        a.sourceEpoch == b.sourceEpoch && a.width == b.width && a.height == b.height;
}
inline bool staleMediaEpoch(const MediaFingerprint& previous, const MediaFingerprint& next) {
    return previous.hasIdentity && next.hasIdentity && previous.videoId == next.videoId &&
        next.sourceEpoch < previous.sourceEpoch;
}
struct Bounds { double left = 0, top = 0, right = 0, bottom = 0; };
inline bool validBounds(const Bounds& value) {
    return std::isfinite(value.left) && std::isfinite(value.top) &&
        std::isfinite(value.right) && std::isfinite(value.bottom) &&
        value.right > value.left && value.bottom > value.top;
}
inline bool anyRegionVisible(const Bounds& video, const std::vector<Bounds>& displays,
                             const std::vector<Bounds>& occluders) {
    // Subtract every higher window, not just the foreground monitor UI.
    // This proves partial visibility without treating covered window pixels as
    // screen output. Keep pathological window layouts bounded and fail closed.
    if (!validBounds(video) || displays.size() > 32 || occluders.size() > 256) return false;
    std::vector<Bounds> remaining;
    const auto intersect = [](const Bounds& a, const Bounds& b) {
        return Bounds{std::max(a.left,b.left), std::max(a.top,b.top),
                      std::min(a.right,b.right), std::min(a.bottom,b.bottom)};
    };
    for (const auto& display : displays) {
        if (!validBounds(display)) return false;
        const auto clipped = intersect(video, display);
        if (validBounds(clipped)) remaining.push_back(clipped);
    }
    for (const auto& cover : occluders) {
        if (!validBounds(cover)) return false;
        std::vector<Bounds> next;
        const auto add = [&](const Bounds& piece) { if (validBounds(piece)) next.push_back(piece); };
        for (const auto& part : remaining) {
            const auto clipped = intersect(part, cover);
            if (!validBounds(clipped)) { next.push_back(part); continue; }
            add({part.left,part.top,part.right,clipped.top});
            add({part.left,clipped.bottom,part.right,part.bottom});
            add({part.left,clipped.top,clipped.left,clipped.bottom});
            add({clipped.right,clipped.top,part.right,clipped.bottom});
        }
        if (next.empty() || next.size() > 1024) return false;
        remaining = std::move(next);
    }
    return !remaining.empty();
}
enum class OuterBoundsMatch { Mismatch, Win32Pixels, VisiblePixels, Win32Dip, VisibleDip };
inline bool matchesBounds(const Bounds& reported, const Bounds& physical, double factor) {
    return std::abs(reported.left * factor - physical.left) <= 2 &&
        std::abs(reported.top * factor - physical.top) <= 2 &&
        std::abs(reported.right * factor - physical.right) <= 2 &&
        std::abs(reported.bottom * factor - physical.bottom) <= 2;
}
inline bool matchesChromiumQuantizedBounds(const Bounds& reported, const Bounds& physical, double factor) {
    if (!std::isfinite(factor) || factor <= 0) return false;
    const auto exact = [](double a, double b) { return std::abs(a - b) < 1e-6; };
    const double logicalWidth = std::ceil(physical.right / factor) - std::floor(physical.left / factor);
    const double logicalHeight = std::ceil(physical.bottom / factor) - std::floor(physical.top / factor);
    if (!exact(reported.right - reported.left, logicalWidth) ||
        !exact(reported.bottom - reported.top, logicalHeight)) return false;
    // ScreenToDIPRect computes an enclosing rectangle size and an independently
    // floor/round-converted origin. Reconstruct those exact integer reports
    // rather than increasing physical edge tolerance for fractional DPI.
    const auto matchesOrigin = [&](bool rounded) {
        const double x = physical.left / factor, y = physical.top / factor;
        return exact(reported.left, rounded ? std::round(x) : std::floor(x)) &&
            exact(reported.top, rounded ? std::round(y) : std::floor(y));
    };
    return matchesOrigin(false) || matchesOrigin(true);
}
inline OuterBoundsMatch outerBoundsMatch(const Bounds& reported, const Bounds& win32,
                                        const Bounds& visible, double dpiScale) {
    // Match a complete convention, never combine the origin of one rectangle
    // with the size of another. Mixed-DPI logical desktop origins require an
    // independently proven mapping and deliberately fall back when unmatched.
    if (matchesBounds(reported, win32, 1)) return OuterBoundsMatch::Win32Pixels;
    if (matchesBounds(reported, visible, 1)) return OuterBoundsMatch::VisiblePixels;
    if (matchesBounds(reported, win32, dpiScale) || matchesChromiumQuantizedBounds(reported, win32, dpiScale))
        return OuterBoundsMatch::Win32Dip;
    if (matchesBounds(reported, visible, dpiScale) || matchesChromiumQuantizedBounds(reported, visible, dpiScale))
        return OuterBoundsMatch::VisibleDip;
    return OuterBoundsMatch::Mismatch;
}
} // namespace veyra::capture::policy
