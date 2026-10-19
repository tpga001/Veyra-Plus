#include "veyra/capture/BrowserAttachmentPolicy.h"
#include <iostream>

int main() {
    using namespace veyra::capture::policy;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool pass, const char* name) {
        ++checks;
        if (!pass) { ++failures; std::cerr << "FAIL " << name << '\n'; }
    };
    check(!firstPresentDeadlineExpired(true, 14999), "first capture retains 15-second startup allowance");
    check(!firstPresentDeadlineExpired(true, 15000), "startup deadline retains its strict greater-than boundary");
    check(firstPresentDeadlineExpired(true, 15001), "missing first present still fails after 15 seconds");
    // Field regression: session 39 had presented 662 frames before epoch 3
    // replaced its metrics window. Temporary zero counters do not undo first present.
    check(!firstPresentDeadlineExpired(false, 25000), "presented capture survives temporary not-ready metrics reset at 25 seconds");
    check(!firstPresentDeadlineExpired(false, 60000), "later resets cannot rearm a completed startup deadline");
    // openCapture sets opening=true and resets openedAt for a new capture.
    check(!firstPresentDeadlineExpired(true, 0), "new capture starts its own first-present allowance");
    check(!firstPresentDeadlineExpired(true, 15000), "new capture has the full original startup allowance");
    check(firstPresentDeadlineExpired(true, 15001), "new capture still fails if its own first present never arrives");
    check(selectViewportCandidate({}, {}) == 0, "no viewport candidate rejects");
    check(selectViewportCandidate({201}, {}) == 201, "single candidate keeps legacy behavior without hit evidence");
    check(selectViewportCandidate({201}, {0, 999, 202}) == 201, "single candidate does not add a hit-test gate");
    check(selectViewportCandidate({201, 202}, {202, 202, 202}) == 202, "duplicate viewports accept three consistent rooted hits");
    check(selectViewportCandidate({202, 201}, {202, 202, 202}) == 202, "candidate enumeration order does not select viewport");
    check(selectViewportCandidate({201, 202}, {201, 201, 201}) == 201, "either candidate needs the same rooted evidence");
    check(selectViewportCandidate({201, 202}, {202, 202, 202, 202}) == 202, "additional consistent internal points accept");
    check(selectViewportCandidate({201, 202}, {}) == 0, "duplicate viewports require rooted evidence");
    check(selectViewportCandidate({201, 202}, {202, 202}) == 0, "fewer than three internal points reject");
    check(selectViewportCandidate({201, 202}, {202, 0, 202}) == 0, "unknown or ambiguous nested hit rejects");
    check(selectViewportCandidate({201, 202}, {101, 101, 101}) == 0, "browser-root hit is not a viewport owner");
    check(selectViewportCandidate({201, 202}, {999, 999, 999}) == 0, "foreign rooted hit rejects");
    check(selectViewportCandidate({201, 202}, {202, 201, 202}) == 0, "contradictory candidate hits reject");
    check(selectViewportCandidate({201, 202, 203}, {201, 202, 203}) == 0, "multiple normal viewports with different evidence reject");
    check(selectViewportCandidate({201, 201}, {201, 201, 201}) == 0, "duplicate candidate IDs reject");
    check(selectViewportCandidate({201, 202, 201}, {202, 202, 202}) == 0, "nonadjacent duplicate candidate IDs reject");
    check(selectViewportCandidate({0}, {}) == 0, "null single candidate rejects");
    check(selectViewportCandidate({201, 0}, {201, 201, 201}) == 0, "null duplicate candidate rejects");
    const BrowserIdentity browser{101, 22, 3000};
    check(viewportDecision({}, browser, 0, 201, false, true) == ViewportDecision::BindFirst, "first report");
    check(viewportDecision(browser, browser, 201, 201, true, false) == ViewportDecision::Keep, "heartbeat keeps viewport");
    check(viewportDecision(browser, browser, 201, 202, false, true) == ViewportDecision::ReplaceDestroyed, "fresh report replaces destroyed same-document viewport");
    check(viewportDecision(browser, browser, 201, 202, false, false) == ViewportDecision::RejectViewport, "tick cannot rebind destroyed viewport");
    check(viewportDecision(browser, browser, 201, 202, true, true) == ViewportDecision::RejectViewport, "live hidden or reparented viewport cannot migrate");
    check(viewportDecision(browser, {102,22,3000}, 201, 202, false, true) == ViewportDecision::RejectBrowser, "different browser root cannot rebind");
    check(viewportDecision(browser, {101,23,3000}, 201, 202, false, true) == ViewportDecision::RejectBrowser, "different PID cannot rebind");
    check(viewportDecision(browser, {101,22,3001}, 201, 202, false, true) == ViewportDecision::RejectBrowser, "reused process identity cannot rebind");
    check(viewportDecision(browser, browser, 201, 0, false, true) == ViewportDecision::RejectViewport, "missing viewport cannot rebind");
    const MediaFingerprint media{"video-a", 2, 1280, 720, true};
    check(sameMedia(media, media), "heartbeat does not reset history");
    check(!sameMedia(media, {"video-a",3,1280,720,true}), "same-size seek epoch resets history");
    check(!sameMedia(media, {"video-b",2,1280,720,true}), "same-size element replacement resets history");
    check(!sameMedia(media, {"video-a",2,1920,1080,true}), "source size change resets history");
    check(staleMediaEpoch(media, {"video-a",1,1280,720,true}), "old epoch rejected independently of request sequence");
    check(!staleMediaEpoch(media, {"video-a",2,1280,720,true}), "equal epoch accepted");
    check(!staleMediaEpoch(media, {"video-a",3,1280,720,true}), "new epoch accepted");
    check(!staleMediaEpoch(media, {"video-b",0,1280,720,true}), "new element has independent epoch");
    check(!sameMedia({"",0,1280,720,false}, {"",0,1920,1080,false}), "legacy source-size fallback resets history");
    check(foregroundAuthority({true,false,false,true,false,true}) == ForegroundAuthority::Browser,
          "first foreground browser can bind without prior monitor permission");
    check(foregroundAuthority({false,true,true,true,false,true}) == ForegroundAuthority::OwnVeyraMonitor,
          "exact own monitor permits only an existing visible bound browser");
    check(foregroundAuthority({false,true,false,true,false,true}) == ForegroundAuthority::Reject,
          "own monitor cannot select a new inactive browser");
    check(foregroundAuthority({false,false,true,true,false,true}) == ForegroundAuthority::Reject,
          "foreign foreground app never grants continuation");
    check(foregroundAuthority({false,true,true,false,false,true}) == ForegroundAuthority::Reject,
          "hidden browser cannot continue beneath own monitor");
    check(foregroundAuthority({false,true,true,true,true,true}) == ForegroundAuthority::Reject,
          "minimized browser cannot continue beneath own monitor");
    check(foregroundAuthority({false,true,true,true,false,false}) == ForegroundAuthority::Reject,
          "fully occluded video cannot continue beneath own monitor");
    check(overlayPlacement(false,true,false) == OverlayPlacement::AboveSource,
          "monitor continuation stays immediately above browser below intervening ordinary apps");
    check(overlayPlacement(false,true,true) == OverlayPlacement::TopOfOrdinaryGroup,
          "topmost predecessor never promotes monitor overlay into topmost group");
    check(overlayPlacement(true,false,false) == OverlayPlacement::TopOfOrdinaryGroup,
          "foreground browser leading ordinary group retains ordinary overlay");
    check(overlayPlacement(false,false,false) == OverlayPlacement::Reject,
          "monitor continuation without source z-order proof fails closed");
    const Bounds videoRegion{0,0,640,360};
    const std::vector<Bounds> displays{{0,0,1920,1080}};
    check(anyRegionVisible(videoRegion, displays, {}), "uncovered video is visible");
    check(anyRegionVisible(videoRegion, displays, {{640,0,1200,360}}), "adjacent monitor UI does not occlude video");
    check(anyRegionVisible(videoRegion, displays, {{0,0,320,360}}), "partial cover retains visible video");
    check(!anyRegionVisible(videoRegion, displays, {{0,0,640,360}}), "single monitor UI fully covers video");
    check(!anyRegionVisible(videoRegion, displays, {{0,0,320,360},{320,0,640,360}}),
          "intervening app plus own monitor jointly cover video");
    check(anyRegionVisible(videoRegion, displays, {{0,0,300,360},{320,0,640,360}}),
          "visible strip between windows remains visible");
    check(!anyRegionVisible({2000,0,2640,360}, displays, {}), "off-screen browser video is not visible");
    check(!anyRegionVisible(videoRegion, {{0,0,320,360}}, {{0,0,320,360}}),
          "cover of the only on-screen section fully occludes video");
    check(!anyRegionVisible({0,0,0,360}, displays, {}), "invalid source rectangle fails closed");
    check(!anyRegionVisible(videoRegion, displays, {{0,0,0,360}}), "invalid occluder fails closed");
    check(!anyRegionVisible(videoRegion, displays, std::vector<Bounds>(257,videoRegion)),
          "unbounded window enumeration fails closed");
    const Bounds win32{82,-11,3851,2171}, visible{93,0,3840,2160};
    check(outerBoundsMatch({62,0,2560,1440},win32,visible,1.5) == OuterBoundsMatch::VisibleDip, "22px invisible resize borders matched explicitly");
    check(outerBoundsMatch(win32,win32,visible,1.5) == OuterBoundsMatch::Win32Pixels, "physical Win32 rectangle");
    check(outerBoundsMatch(visible,win32,visible,1.5) == OuterBoundsMatch::VisiblePixels, "physical visible rectangle");
    check(outerBoundsMatch({62,0,2574,1440},win32,visible,1.5) == OuterBoundsMatch::Mismatch, "wrong size cannot borrow border tolerance");
    check(outerBoundsMatch({55,-7,2560,1440},win32,visible,1.5) == OuterBoundsMatch::Mismatch, "cannot combine Win32 origin and visible size");
    check(outerBoundsMatch({-1280,0,-640,720},{-1920,0,-960,1080},{-1920,0,-960,1080},1.5) == OuterBoundsMatch::Win32Dip, "negative coordinates remain supported when mapping is exact");
    check(outerBoundsMatch({2560,0,3200,720},{3840,0,4480,720},{3840,0,4480,720},1) == OuterBoundsMatch::Mismatch, "unproven mixed-DPI origin falls back");
    const Bounds fractional{1,1,1000,1000};
    check(outerBoundsMatch({1,1,573,573}, fractional, fractional, 1.75) == OuterBoundsMatch::Win32Dip,
          "Chromium rounded origin and enclosing size at 175 percent");
    check(outerBoundsMatch({0,0,572,572}, fractional, fractional, 1.75) == OuterBoundsMatch::Win32Dip,
          "Chromium floored origin and enclosing size at 175 percent");
    check(outerBoundsMatch({1,1,574,573}, fractional, fractional, 1.75) == OuterBoundsMatch::Mismatch,
          "adjacent incorrect quantized size is rejected");
    check(outerBoundsMatch({2,1,574,573}, fractional, fractional, 1.75) == OuterBoundsMatch::Mismatch,
          "adjacent incorrect quantized origin is rejected");
    check(outerBoundsMatch({80,40,880,440},{100,50,1100,550},{100,50,1100,550},1.25) == OuterBoundsMatch::Win32Dip,
          "fractional 125 percent complete rectangle");
    std::cout << "browser_attachment_policy checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
