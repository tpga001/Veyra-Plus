#include "veyra/diagnostics/DisplayFrameMetrics.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace veyra::diagnostics;
namespace {
unsigned checks = 0, failures = 0;
void check(bool okay, const char* label) {
    ++checks; if (!okay) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
bool near(std::optional<double> actual, double expected) {
    return actual && std::abs(*actual - expected) < 0.00001;
}
const std::string header = "Application,ProcessID,SwapChainAddress,FrameType,CPUStartQPCTime,DisplayLatency,DisplayedTime,FrameTime";
DisplayTarget target(uint64_t chain = 0x123) {
    DisplayTarget result; result.pid = 123; result.sessionId = 7; result.epoch = 3; result.revision = 9;
    result.outputSwapChain = chain; result.generation = 1; return result;
}
DisplayFrameRecord frame(double start, double duration, uint64_t chain = 0x123, const std::string& type = "Application") {
    DisplayFrameRecord result;
    result.application = "veyra_qml_ui.exe"; result.pid = 123; result.swapChain = chain; result.frameType = type;
    result.cpuStartQpcMs = start; result.displayLatencyMs = 0; result.displayStartQpcMs = start;
    result.displayedMs = duration; result.kind = DisplayRecordKind::Displayed; return result;
}
DisplayFrameRecord nonDisplayed(double at, DisplayRecordKind kind) {
    auto result = frame(at, 0); result.kind = kind; result.displayLatencyMs.reset(); result.displayStartQpcMs.reset(); return result;
}
void addFrames(DisplayMetricsWindow& window, unsigned count, double first, double duration, uint64_t chain = 0x123) {
    for (unsigned i = 0; i < count; ++i) window.add(frame(first + double(i) * duration, duration, chain,
        i % 2 ? "Intel XeSS-FG" : "Application"));
}
void parsing() {
    DisplayCsvParser parser; std::string error;
    check(!parser.parseRecord("x", &error) && !error.empty(), "row requires an accepted header");
    check(parser.parseHeader(header, &error) && parser.ready() && error.empty(), "all required columns accepted");
    auto row = parser.parseRecord("veyra_qml_ui.exe,123,0xABC,Intel XeSS-FG,1000,7,16.6666667,0", &error);
    check(row && error.empty() && row->swapChain == 0xabc && row->pid == 123, "hex chain and unsigned PID parsed");
    check(row && near(row->displayStartQpcMs, 1007) && row->kind == DisplayRecordKind::Displayed, "display start is CPU QPC plus display latency");
    check(row && row->frameType == "Intel XeSS-FG" && row->displayedMs > 16, "generated zero-CPU frames retained");
    row = parser.parseRecord("\"veyra,\"\"quoted\"\".exe\",123,2748,\"FG, named\",1000,0,16,unused\r\n", &error);
    check(row && row->application == "veyra,\"quoted\".exe" && row->frameType == "FG, named", "quoted commas and doubled quotes decoded");
    row = parser.parseRecord("veyra_qml_ui.exe,123,0XABC,Application,1000,NA,NA,0", &error);
    check(row && row->kind == DisplayRecordKind::NotDisplayed && !row->displayStartQpcMs, "NA is an undisplayed Present without invented display coordinates");
    row = parser.parseRecord("veyra_qml_ui.exe,123,0,Application,1000,NA,0,0", &error);
    check(row && row->kind == DisplayRecordKind::Unresolved && row->swapChain == 0, "zero duration unresolved; zero chain preserved for rejection at binding");
    row = parser.parseRecord("veyra_qml_ui.exe,123,123,Application,1000,0,16,", &error);
    check(row.has_value(), "trailing empty extra field parsed");
    for (const auto* malformed : {
        "\"veyra,123,0x123,Application,1000,0,16,0",
        "\"veyra\"x,123,0x123,Application,1000,0,16,0",
        "vey\"ra,123,0x123,Application,1000,0,16,0",
        "veyra_qml_ui.exe,123,0x123,Application,1000,0,16",
        "veyra_qml_ui.exe,123,0x123,Application,1000,0,16,0,extra",
        "veyra_qml_ui.exe,-1,0x123,Application,1000,0,16,0",
        "veyra_qml_ui.exe,0,0x123,Application,1000,0,16,0",
        "veyra_qml_ui.exe,4294967296,0x123,Application,1000,0,16,0",
        "veyra_qml_ui.exe,123,0x10000000000000000,Application,1000,0,16,0",
        "veyra_qml_ui.exe,123,-1,Application,1000,0,16,0",
        "veyra_qml_ui.exe,123,0x,Application,1000,0,16,0",
        "veyra_qml_ui.exe,123,123junk,Application,1000,0,16,0",
        "veyra_qml_ui.exe,123,123,Application,NA,0,16,0",
        "veyra_qml_ui.exe,123,123,Application,nan,0,16,0",
        "veyra_qml_ui.exe,123,123,Application,1000,-1,16,0",
        "veyra_qml_ui.exe,123,123,Application,1000,NA,16,0",
        "veyra_qml_ui.exe,123,123,Application,1000,0,-1,0",
        "veyra_qml_ui.exe,123,123,Application,1000,0,inf,0",
        "veyra_qml_ui.exe,123,123,Application,1000,0,NaN,0",
        "veyra_qml_ui.exe,123,123,Application,1000,0,16junk,0",
        "veyra_qml_ui.exe,123,123,Application,1000,0,na,0",
        ",123,123,Application,1000,0,16,0",
        "veyra_qml_ui.exe,123,123,,1000,0,16,0"}) {
        check(!parser.parseRecord(malformed, &error) && !error.empty(), "malformed row rejected with diagnostic");
    }
    check(parser.parseHeader(std::string("\xEF\xBB\xBF") + header + "\r\n", &error), "UTF-8 BOM and CRLF header accepted");
    check(parser.parseHeader("\"Application\",\"ProcessID\",SwapChainAddress,FrameType,CPUStartQPCTime,DisplayLatency,DisplayedTime", &error), "quoted header names accepted");
    for (const auto* bad : {
        "Application,ProcessID,SwapChainAddress,FrameType,CPUStartQPCTime,DisplayLatency",
        "Application,ProcessID,SwapChainAddress,FrameType,CPUStartQPCTime,DisplayLatency,DisplayedTime,Application",
        "Application,ProcessID,SwapChainAddress,FrameType,CPUStartQPCTime,DisplayLatency,DisplayedTime,",
        "\"Application,ProcessID,SwapChainAddress,FrameType,CPUStartQPCTime,DisplayLatency,DisplayedTime"}) {
        check(!parser.parseHeader(bad, &error) && !parser.ready() && !error.empty(), "invalid header also discards the previously accepted schema");
    }
}
void ratesAndCounters() {
    DisplayMetricsWindow window; window.reset(target(), 1000);
    check(!window.snapshot(1000).known(), "new mapped window starts unknown");
    addFrames(window, 120, 1000, 1000.0 / 60);
    window.add(nonDisplayed(1100, DisplayRecordKind::NotDisplayed));
    window.add(nonDisplayed(1200, DisplayRecordKind::Unresolved));
    auto result = window.snapshot(3000);
    check(result.known() && near(result.fps, 60) && near(result.onePercentLowFps, 60), "positive display durations yield 60 and 60 low including generated frames");
    check(result.displayedFrames == 120 && result.notDisplayedPresents == 1 && result.unresolvedPresents == 1, "displayed, not displayed and unresolved stay distinct");
    window.add(frame(1000, 1000.0 / 60));
    check(window.snapshot(3000).displayedFrames == 120, "identical completed record is not counted twice");
    check(window.displayedDurations(3000).size() == 120, "sparkline consists of actual display durations");
    window.reset(target(), 0); addFrames(window, 120, 0, 1000.0 / 120);
    check(near(window.snapshot(1000).fps, 120), "FPS is never clamped to a nominal 60 Hz target");
    window.reset(target(), 0);
    double at = 0;
    for (unsigned i = 0; i < 100; ++i) { const double duration = i ? 10 : 100; window.add(frame(at, duration)); at += duration; }
    result = window.snapshot(at);
    check(near(result.onePercentLowFps, 10), "1 percent low uses the worst-duration mean rather than percentile reciprocal");
    check(near(result.fps, 100000.0 / 1090), "slow frame remains in average display duration");
    window.reset(target(), 0); at = 0;
    for (unsigned i = 0; i < 200; ++i) { const double duration = i == 0 ? 100 : i == 1 ? 50 : 10; window.add(frame(at, duration)); at += duration; }
    check(near(window.snapshot(at).onePercentLowFps, 1000.0 / 75), "low averages both slowest frames when ceil N percent is two");
    window.reset(target(), 0); addFrames(window, 99, 0, 10);
    check(window.snapshot(990).known() && !window.snapshot(990).onePercentLowFps, "low remains unknown until 100 valid frames");
    window.reset(target(), 0); addFrames(window, 600, 0, 10);
    result = window.snapshot(6000);
    check(result.displayedFrames == 500 && near(result.fps, 100), "window retains only the most recent five seconds");
    const auto durations = window.displayedDurations(6000);
    check(durations.size() == 500 && std::all_of(durations.begin(), durations.end(), [](double value) { return value == 10; }), "duration history obeys the same rolling boundary");
}
void readinessAndInvalidation() {
    DisplayMetricsWindow window; window.reset(target(), 0); addFrames(window, 29, 0, 20);
    check(!window.snapshot(580).known(), "29 frames cannot produce FPS despite sufficient time");
    window.reset(target(), 0); addFrames(window, 30, 0, 10);
    check(!window.snapshot(300).known(), "30 frames do not bypass the half-second observation boundary");
    window.reset(target(), 0); addFrames(window, 30, 0, 1000.0 / 60);
    check(window.snapshot(500).known(), "30 frames and half a second meet FPS warmup");
    check(!window.snapshot(2501).known() && window.snapshot(2501).state == DisplayMeasurementState::Stale, "two seconds without a fresh completed event is stale");
    check(window.displayedDurations(2501).empty(), "stale sample does not refill a live sparkline");
    window.markUnavailable("trace events lost");
    check(window.snapshot(500).state == DisplayMeasurementState::Unavailable && !window.snapshot(500).fps, "trace loss invalidates a previously valid rate");
    window.reset(target(), 1000); addFrames(window, 30, 1000, 1000.0 / 60);
    check(window.snapshot(1500).known(), "fresh collector reset can recover after trace loss");
    auto invalid = frame(1490, 10); invalid.displayedMs = std::numeric_limits<double>::infinity();
    check(window.add(invalid) == DisplayRecordAdmission::Invalid, "nonfinite manual records cannot bypass parser contracts");
    check(!window.snapshot(1500).known() && window.snapshot(1500).invalidRecords == 1, "invalid matching records prevent performance claims");
    window.reset(target(), 0); addFrames(window, 30, 0, 1000.0 / 60);
    check(!window.snapshot(std::numeric_limits<double>::quiet_NaN()).known(), "nonfinite now coordinate stays unknown");
    window.reset(target(), -1);
    check(window.snapshot(0).state == DisplayMeasurementState::Unavailable, "negative reset QPC rejected");
    auto noSession = target(); noSession.sessionId = 0; window.reset(noSession, 0);
    check(!window.snapshot(1000).known(), "output cannot be labelled without an owned source session");
    window.reset(target(), 1000);
    auto old = frame(900, 16); old.displayLatencyMs = 200; old.displayStartQpcMs = 1100;
    check(window.add(old) == DisplayRecordAdmission::Ignored, "present submitted before reset cannot become new-session display evidence");
    check(window.add(frame(1000, 5000)) == DisplayRecordAdmission::Accepted && !window.snapshot(1500).known(), "future unfinished span is retained but cannot claim current FPS");
}
void identityAndMapping() {
    DisplayMetricsWindow window; window.reset(target(), 0);
    auto foreignPid = frame(0, 20); foreignPid.pid = 124;
    auto foreignApp = frame(0, 20); foreignApp.application = "chrome.exe";
    check(window.add(foreignPid) == DisplayRecordAdmission::Ignored && window.add(foreignApp) == DisplayRecordAdmission::Ignored, "PID and application must both match");
    for (unsigned i = 0; i < 120; ++i) window.add(frame(i * 10, 10, 0x456));
    check(!window.snapshot(1200).known(), "active UI or preview swapchains never replace the confirmed video chain");
    auto uppercase = frame(0, 20); uppercase.application = "VEYRA_QML_UI.EXE";
    check(window.add(uppercase) == DisplayRecordAdmission::Accepted, "Windows application basename comparison ignores ASCII case");
    std::vector<DisplayFrameRecord> reverse;
    for (unsigned i = 0; i < 120; ++i) reverse.push_back(frame(1000 + i * 1000.0 / 60, 1000.0 / 60));
    window.reset(target(), 1000); for (auto it = reverse.rbegin(); it != reverse.rend(); ++it) window.add(*it);
    check(near(window.snapshot(3000).fps, 60), "out-of-order completed events give the same window rate");
    auto changed = target(); ++changed.epoch; window.reset(changed, 3000);
    check(!window.snapshot(3000).known() && window.snapshot(3000).target.epoch == 4, "temporal epoch reset flushes the previous rate");
    ++changed.sessionId; ++changed.revision; ++changed.generation; changed.outputSwapChain = 0x456;
    window.reset(changed, 3000);
    check(window.add(reverse.back()) == DisplayRecordAdmission::Ignored && !window.snapshot(3500).known(), "new session/revision/output generation cannot reuse an old event");
    window.reset(target(0), 0); window.setExpectedGeneratedFrameType("NVIDIA DLSS-FG");
    addFrames(window, 240, 0, 5, 0x999);
    check(!window.mappingCandidate(1200), "wrong provider marker cannot identify output even at a higher event rate");
    // Explicit provider-marked events; zero chain and the UI's higher rate cannot identify output.
    window.reset(target(0), 0); window.setExpectedGeneratedFrameType("Intel XeSS-FG");
    for (unsigned i = 0; i < 23; ++i) window.add(frame(i * 50, 50, 0x777, "Intel XeSS-FG"));
    check(window.mappingCandidate(1150) == std::optional<uint64_t>(0x777), "one provider-marked nonzero chain stable for one second can be suggested");
    check(!window.snapshot(1150).known() && window.snapshot(1150).state == DisplayMeasurementState::Unmapped, "candidate suggestion never silently confirms output mapping");
    window.add(frame(1100, 25, 0x888, "Intel XeSS-FG"));
    check(!window.mappingCandidate(1150), "a second provider-marked chain makes the mapping ambiguous");
    window.reset(target(0), 0);
    for (unsigned i = 0; i < 23; ++i) window.add(frame(i * 50, 50, 0, "Intel XeSS-FG"));
    check(!window.mappingCandidate(1150), "zero-address generated events cannot identify the output");
    window.reset(target(0), 0); window.setExpectedGeneratedFrameType("Application");
    for (unsigned i = 0; i < 23; ++i) window.add(frame(i * 50, 50, 0x777));
    check(!window.mappingCandidate(1150), "Application marker cannot authorize a proxy mapping");
    window.reset(target(0), 0); window.setExpectedGeneratedFrameType("Intel XeSS-FG");
    for (unsigned i = 0; i < 20; ++i) window.add(frame(i * 50, 50, 0x777, "Intel XeSS-FG"));
    check(!window.mappingCandidate(1000), "less than one second of provider-event stability is not enough");
    window.add(frame(1000, 50, 0x777, "Intel XeSS-FG"));
    check(window.mappingCandidate(1050) && !window.mappingCandidate(4000), "stale provider evidence cannot authorize a new mapping");
    window.markUnavailable("trace stopped"); check(!window.mappingCandidate(1050), "failed trace cannot provide a mapping suggestion");
    window.reset(target(0x777), 2000); addFrames(window, 120, 2000, 1000.0 / 60, 0x777);
    check(near(window.snapshot(4000).fps, 60), "explicitly confirmed output starts a fresh measured window");
}
} // namespace
int main() {
    parsing(); ratesAndCounters(); readinessAndInvalidation(); identityAndMapping();
    std::cout << "DISPLAY_FRAME_METRICS checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
