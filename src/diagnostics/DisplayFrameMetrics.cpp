#include "veyra/diagnostics/DisplayFrameMetrics.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <functional>

namespace veyra::diagnostics {
namespace {
constexpr size_t kMaximumRecords = 16384;
void failure(std::string* out, std::string_view message) { if (out) *out = message; }
std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}
bool equalAsciiInsensitive(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(static_cast<unsigned char>(a[i])) != lower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}
bool csvFields(std::string_view csv, std::vector<std::string>& out, std::string* error) {
    if (!csv.empty() && csv.back() == '\n') csv.remove_suffix(1);
    if (!csv.empty() && csv.back() == '\r') csv.remove_suffix(1);
    size_t at = 0;
    for (;;) {
        std::string field;
        if (at < csv.size() && csv[at] == '"') {
            ++at; bool closed = false;
            while (at < csv.size()) {
                const char c = csv[at++];
                if (c != '"') { field.push_back(c); continue; }
                if (at < csv.size() && csv[at] == '"') { field.push_back('"'); ++at; continue; }
                closed = true; break;
            }
            if (!closed || (at < csv.size() && csv[at] != ',')) {
                failure(error, "Malformed quoted CSV field"); return false;
            }
        } else {
            while (at < csv.size() && csv[at] != ',') {
                const char c = csv[at++];
                if (c == '"' || c == '\r' || c == '\n') {
                    failure(error, "Quote or newline in an unquoted CSV field"); return false;
                }
                field.push_back(c);
            }
        }
        out.push_back(std::move(field));
        if (at == csv.size()) return true;
        ++at;
        // A trailing comma contributes one empty final field on the next iteration.
    }
}
template<class T> bool unsignedNumber(std::string_view value, T& result, bool hexAllowed = false) {
    value = trim(value); int base = 10;
    if (hexAllowed && value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2); base = 16;
    }
    if (value.empty()) return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
bool positiveOrZeroNumber(std::string_view value, double& result) {
    value = trim(value);
    if (value.empty()) return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && std::isfinite(result) && result >= 0;
}
bool validRecord(const DisplayFrameRecord& record) {
    if (!record.pid || record.application.empty() || record.frameType.empty() ||
        !std::isfinite(record.cpuStartQpcMs) || record.cpuStartQpcMs < 0 ||
        !std::isfinite(record.displayedMs) || record.displayedMs < 0) return false;
    if (record.displayLatencyMs && (!std::isfinite(*record.displayLatencyMs) || *record.displayLatencyMs < 0)) return false;
    if (record.displayStartQpcMs && (!record.displayLatencyMs || !std::isfinite(*record.displayStartQpcMs) ||
        std::abs(*record.displayStartQpcMs - (record.cpuStartQpcMs + *record.displayLatencyMs)) > 0.000001)) return false;
    if (record.kind == DisplayRecordKind::Displayed)
        return record.displayedMs > 0 && record.displayStartQpcMs &&
            std::isfinite(*record.displayStartQpcMs + record.displayedMs);
    return (record.kind == DisplayRecordKind::NotDisplayed || record.kind == DisplayRecordKind::Unresolved) && record.displayedMs == 0;
}
double recordTime(const DisplayFrameRecord& record) {
    return record.kind == DisplayRecordKind::Displayed ? *record.displayStartQpcMs : record.cpuStartQpcMs;
}
bool sameRecord(const DisplayFrameRecord& a, const DisplayFrameRecord& b) {
    return a.pid == b.pid && a.swapChain == b.swapChain && a.frameType == b.frameType && a.kind == b.kind &&
        a.cpuStartQpcMs == b.cpuStartQpcMs && a.displayLatencyMs == b.displayLatencyMs &&
        a.displayStartQpcMs == b.displayStartQpcMs && a.displayedMs == b.displayedMs;
}
void appendBounded(std::vector<DisplayFrameRecord>& records, const DisplayFrameRecord& record) {
    if (std::any_of(records.begin(), records.end(), [&](const auto& old) { return sameRecord(old, record); })) return;
    records.push_back(record);
    double latest = recordTime(record);
    for (const auto& old : records) latest = std::max(latest, recordTime(old));
    std::erase_if(records, [&](const auto& old) { return recordTime(old) < latest - DisplayMetricsWindow::kWindowMs; });
}
} // namespace

bool DisplayCsvParser::parseHeader(std::string_view csv, std::string* error) {
    columns_.clear(); width_ = 0; if (error) error->clear();
    if (csv.starts_with("\xEF\xBB\xBF")) csv.remove_prefix(3);
    std::vector<std::string> fields;
    if (!csvFields(csv, fields, error)) return false;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].empty() || !columns_.emplace(fields[i], i).second) {
            columns_.clear(); failure(error, "Empty or duplicate CSV column"); return false;
        }
    }
    for (const auto* required : {"Application", "ProcessID", "SwapChainAddress", "FrameType", "CPUStartQPCTime", "DisplayLatency", "DisplayedTime"}) {
        if (!columns_.contains(required)) {
            columns_.clear(); failure(error, std::string("Missing required CSV column: ") + required); return false;
        }
    }
    width_ = fields.size(); return true;
}
std::optional<DisplayFrameRecord> DisplayCsvParser::parseRecord(std::string_view csv, std::string* error) const {
    if (error) error->clear();
    if (!ready()) { failure(error, "CSV header has not been accepted"); return {}; }
    std::vector<std::string> fields;
    if (!csvFields(csv, fields, error)) return {};
    if (fields.size() != width_) { failure(error, "CSV record width differs from its header"); return {}; }
    const auto field = [&](const char* key) -> std::string_view { return fields[columns_.at(key)]; };
    DisplayFrameRecord record;
    record.application = field("Application"); record.frameType = field("FrameType");
    if (record.application.empty() || record.frameType.empty() || !unsignedNumber(field("ProcessID"), record.pid) || !record.pid ||
        !unsignedNumber(field("SwapChainAddress"), record.swapChain, true) ||
        !positiveOrZeroNumber(field("CPUStartQPCTime"), record.cpuStartQpcMs)) {
        failure(error, "Invalid application, PID, swapchain, frame type or CPU QPC value"); return {};
    }
    const auto latency = trim(field("DisplayLatency"));
    if (latency != "NA") {
        double value = 0;
        if (!positiveOrZeroNumber(latency, value)) { failure(error, "Invalid display latency"); return {}; }
        record.displayLatencyMs = value;
        const double start = record.cpuStartQpcMs + value;
        if (!std::isfinite(start)) { failure(error, "Display QPC overflow"); return {}; }
        record.displayStartQpcMs = start;
    }
    const auto duration = trim(field("DisplayedTime"));
    if (duration == "NA") record.kind = DisplayRecordKind::NotDisplayed;
    else {
        if (!positiveOrZeroNumber(duration, record.displayedMs)) { failure(error, "Invalid displayed duration"); return {}; }
        record.kind = record.displayedMs > 0 ? DisplayRecordKind::Displayed : DisplayRecordKind::Unresolved;
        if (record.kind == DisplayRecordKind::Displayed && !record.displayStartQpcMs) {
            failure(error, "Displayed frame has no display-time coordinates"); return {};
        }
    }
    if (!validRecord(record)) { failure(error, "Inconsistent or overflowing display record"); return {}; }
    return record;
}

bool DisplayMetricsWindow::matchesProcess(const DisplayFrameRecord& record) const {
    return target_.pid && record.pid == target_.pid && equalAsciiInsensitive(record.application, target_.application);
}
void DisplayMetricsWindow::reset(DisplayTarget target, double beginQpcMs) {
    target_ = std::move(target); beginQpcMs_ = beginQpcMs;
    records_.clear(); mappingRecords_.clear(); invalidRecords_ = 0; unavailableReason_.clear();
    if (!std::isfinite(beginQpcMs_) || beginQpcMs_ < 0 || !target_.pid || !target_.sessionId || target_.application.empty())
        unavailableReason_ = "Invalid display target or reset QPC boundary";
}
void DisplayMetricsWindow::markUnavailable(std::string reason) {
    unavailableReason_ = reason.empty() ? "Display collector unavailable" : std::move(reason);
}
void DisplayMetricsWindow::setExpectedGeneratedFrameType(std::string frameType) {
    if (frameType == expectedGeneratedType_) return;
    expectedGeneratedType_ = std::move(frameType); mappingRecords_.clear();
    if (equalAsciiInsensitive(expectedGeneratedType_, "Application") || equalAsciiInsensitive(expectedGeneratedType_, "Unknown") ||
        equalAsciiInsensitive(expectedGeneratedType_, "Not Set")) expectedGeneratedType_.clear();
}
DisplayRecordAdmission DisplayMetricsWindow::add(const DisplayFrameRecord& record) {
    if (!unavailableReason_.empty() || !matchesProcess(record)) return DisplayRecordAdmission::Ignored;
    if (target_.outputSwapChain && record.swapChain != target_.outputSwapChain) return DisplayRecordAdmission::Ignored;
    if (!validRecord(record)) { ++invalidRecords_; return DisplayRecordAdmission::Invalid; }
    if (record.cpuStartQpcMs < beginQpcMs_) return DisplayRecordAdmission::Ignored;
    if (!target_.outputSwapChain) {
        if (record.swapChain && record.kind == DisplayRecordKind::Displayed && !expectedGeneratedType_.empty() &&
            record.frameType == expectedGeneratedType_) {
            appendBounded(mappingRecords_, record);
            if (mappingRecords_.size() > kMaximumRecords) markUnavailable("Display mapping record bound exceeded");
        }
        return DisplayRecordAdmission::Ignored;
    }
    appendBounded(records_, record);
    if (records_.size() > kMaximumRecords) {
        markUnavailable("Display record bound exceeded"); return DisplayRecordAdmission::Invalid;
    }
    return DisplayRecordAdmission::Accepted;
}
std::optional<uint64_t> DisplayMetricsWindow::mappingCandidate(double nowQpcMs) const {
    if (!unavailableReason_.empty() || target_.outputSwapChain || expectedGeneratedType_.empty() ||
        !std::isfinite(nowQpcMs) || nowQpcMs < beginQpcMs_) return {};
    uint64_t chain = 0, count = 0;
    double first = std::numeric_limits<double>::infinity(), last = 0, lastEnd = 0;
    const double cutoff = std::max(beginQpcMs_, nowQpcMs - kWindowMs);
    for (const auto& record : mappingRecords_) {
        const double start = *record.displayStartQpcMs, end = start + record.displayedMs;
        if (start < cutoff || end > nowQpcMs + 0.01) continue;
        if (chain && chain != record.swapChain) return {};
        chain = record.swapChain; ++count;
        first = std::min(first, start); last = std::max(last, start); lastEnd = std::max(lastEnd, end);
    }
    if (chain && count >= 2 && last - first >= kMappingStableMs && nowQpcMs - lastEnd < kStaleMs) return chain;
    return {};
}
DisplayMetricsSnapshot DisplayMetricsWindow::snapshot(double nowQpcMs) const {
    DisplayMetricsSnapshot result; result.target = target_; result.invalidRecords = invalidRecords_;
    result.windowBeginQpcMs = std::max(beginQpcMs_, nowQpcMs - kWindowMs); result.windowEndQpcMs = nowQpcMs;
    if (!unavailableReason_.empty() || !std::isfinite(nowQpcMs) || nowQpcMs < beginQpcMs_) {
        result.state = DisplayMeasurementState::Unavailable;
        result.reason = unavailableReason_.empty() ? "Invalid current QPC coordinate" : unavailableReason_; return result;
    }
    if (!target_.outputSwapChain) { result.reason = "Video output swapchain is not confirmed"; return result; }
    result.state = DisplayMeasurementState::Warming;
    std::vector<double> durations;
    double first = std::numeric_limits<double>::infinity(), last = 0, summed = 0;
    for (const auto& record : records_) {
        const double at = recordTime(record);
        if (at > nowQpcMs) continue;
        if (record.kind == DisplayRecordKind::Displayed) {
            const double end = at + record.displayedMs;
            // Only completed display spans are evidence; future spans never fill warmup.
            if (end > nowQpcMs + 0.01) continue;
            if (!result.lastDisplayQpcMs || end > *result.lastDisplayQpcMs) result.lastDisplayQpcMs = end;
            if (at < result.windowBeginQpcMs) continue;
            durations.push_back(record.displayedMs); summed += record.displayedMs;
            first = std::min(first, at); last = std::max(last, end); ++result.displayedFrames;
        } else if (at >= result.windowBeginQpcMs) {
            if (record.kind == DisplayRecordKind::NotDisplayed) ++result.notDisplayedPresents;
            else ++result.unresolvedPresents;
        }
    }
    result.displayedSpanMs = durations.empty() ? 0 : last - first;
    result.sampleAgeMs = result.lastDisplayQpcMs ? std::max(0.0, nowQpcMs - *result.lastDisplayQpcMs) : 0;
    if (result.lastDisplayQpcMs && result.sampleAgeMs >= kStaleMs) {
        result.state = DisplayMeasurementState::Stale; result.reason = "No recent completed display event"; return result;
    }
    if (result.invalidRecords) { result.state = DisplayMeasurementState::Unavailable; result.reason = "Invalid display records require a fresh capture window"; return result; }
    if (result.displayedFrames < kMinimumFpsFrames || result.displayedSpanMs + 0.01 < kMinimumFpsSpanMs || summed <= 0) {
        result.reason = "Collecting display samples"; return result;
    }
    result.state = DisplayMeasurementState::Measured; result.fps = double(result.displayedFrames) * 1000.0 / summed;
    if (result.displayedFrames >= kMinimumLowFrames) {
        std::sort(durations.begin(), durations.end(), std::greater<double>{});
        const size_t count = std::max<size_t>(1, size_t(std::ceil(double(durations.size()) * 0.01)));
        double slow = 0; for (size_t i = 0; i < count; ++i) slow += durations[i];
        result.onePercentLowFps = double(count) * 1000.0 / slow;
    }
    return result;
}
std::vector<double> DisplayMetricsWindow::displayedDurations(double nowQpcMs) const {
    if (!snapshot(nowQpcMs).known()) return {};
    std::vector<std::pair<double, double>> ordered;
    const double cutoff = std::max(beginQpcMs_, nowQpcMs - kWindowMs);
    for (const auto& record : records_) {
        if (record.kind != DisplayRecordKind::Displayed) continue;
        const double at = *record.displayStartQpcMs;
        if (at >= cutoff && at + record.displayedMs <= nowQpcMs + 0.01) ordered.emplace_back(at, record.displayedMs);
    }
    std::sort(ordered.begin(), ordered.end());
    std::vector<double> result; result.reserve(ordered.size());
    for (const auto& item : ordered) result.push_back(item.second);
    return result;
}
} // namespace veyra::diagnostics
