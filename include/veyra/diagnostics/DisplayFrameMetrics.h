#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace veyra::diagnostics {
// PresentMon --qpc_time_ms coordinates, never wall-clock or CPU frame times.
enum class DisplayRecordKind { Displayed, NotDisplayed, Unresolved };
struct DisplayFrameRecord {
    std::string application;
    uint32_t pid = 0;
    uint64_t swapChain = 0;
    std::string frameType;
    double cpuStartQpcMs = 0;
    std::optional<double> displayLatencyMs;
    std::optional<double> displayStartQpcMs;
    double displayedMs = 0;
    DisplayRecordKind kind = DisplayRecordKind::Unresolved;
};
// A complete CSV record must be supplied; the transport owns partial-line buffering.
// Quoted commas and doubled quotes are decoded; malformed quoting/width is rejected.
class DisplayCsvParser {
    std::unordered_map<std::string, size_t> columns_;
    size_t width_ = 0;
public:
    bool parseHeader(std::string_view csv, std::string* error = nullptr);
    std::optional<DisplayFrameRecord> parseRecord(std::string_view csv, std::string* error = nullptr) const;
    bool ready() const { return width_ != 0; }
};
struct DisplayTarget {
    uint32_t pid = 0;
    uint64_t sessionId = 0, epoch = 0, revision = 0;
    uint64_t outputSwapChain = 0, generation = 0;
    std::string application = "veyra_qml_ui.exe";
    bool operator==(const DisplayTarget&) const = default;
};
enum class DisplayMeasurementState { Unmapped, Warming, Measured, Stale, Unavailable };
struct DisplayMetricsSnapshot {
    DisplayTarget target;
    DisplayMeasurementState state = DisplayMeasurementState::Unmapped;
    std::optional<double> fps, onePercentLowFps;
    uint64_t displayedFrames = 0, notDisplayedPresents = 0, unresolvedPresents = 0;
    uint64_t invalidRecords = 0;
    double windowBeginQpcMs = 0, windowEndQpcMs = 0;
    double displayedSpanMs = 0, sampleAgeMs = 0;
    std::optional<double> lastDisplayQpcMs;
    std::string reason;
    bool known() const { return state == DisplayMeasurementState::Measured && fps.has_value(); }
};
enum class DisplayRecordAdmission { Ignored, Accepted, Invalid };
class DisplayMetricsWindow {
public:
    static constexpr double kWindowMs = 5000.0, kStaleMs = 2000.0;
    static constexpr uint64_t kMinimumFpsFrames = 30, kMinimumLowFrames = 100;
    static constexpr double kMinimumFpsSpanMs = 500.0, kMappingStableMs = 1000.0;
    // reset is required for every source/session/epoch/revision/output generation.
    // Events beginning before this same-QPC boundary can never enter the new window.
    void reset(DisplayTarget target, double beginQpcMs);
    const DisplayTarget& target() const { return target_; }
    DisplayRecordAdmission add(const DisplayFrameRecord& record);
    DisplayMetricsSnapshot snapshot(double nowQpcMs) const;
    // Invalidate on helper failure, permission failure or lost ETW events. Only a
    // fresh reset can recover; retaining a previous number would mislabel evidence.
    void markUnavailable(std::string reason);
    // Discovery is deliberately separate from confirmation. With no mapped output,
    // only one nonzero chain carrying this exact generated-frame type for >=1 s
    // may be suggested. Caller must confirm and reset with that chain; observing
    // a candidate never makes FPS known. No highest-FPS/UI-activity heuristic.
    void setExpectedGeneratedFrameType(std::string frameType);
    std::optional<uint64_t> mappingCandidate(double nowQpcMs) const;
    // Raw display durations for a sparkline, not reciprocal averaged submit FPS.
    std::vector<double> displayedDurations(double nowQpcMs) const;
private:
    DisplayTarget target_;
    double beginQpcMs_ = 0;
    std::vector<DisplayFrameRecord> records_;
    std::vector<DisplayFrameRecord> mappingRecords_;
    std::string expectedGeneratedType_, unavailableReason_;
    uint64_t invalidRecords_ = 0;
    bool matchesProcess(const DisplayFrameRecord& record) const;
};
} // namespace veyra::diagnostics
