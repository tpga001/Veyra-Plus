#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace veyra::diagnostics {

struct NativePaintSnapshot {
    std::uint64_t enters{};
    std::uint64_t exits{};
    std::uint32_t depth{};
    std::uint64_t startMs{};
    std::uintptr_t hwnd{};
    std::uint32_t tid{};
};

class NativePaintScope;

// Diagnostic snapshots are best effort while another thread enters or exits.
// The outermost active scope owns the start time and window/thread identity.
class NativePaintCounter {
public:
    [[nodiscard]] NativePaintSnapshot snapshot() const noexcept {
        return {enters_.load(std::memory_order_relaxed),
                exits_.load(std::memory_order_relaxed),
                depth_.load(std::memory_order_acquire),
                startMs_.load(std::memory_order_relaxed),
                hwnd_.load(std::memory_order_relaxed),
                tid_.load(std::memory_order_relaxed)};
    }

private:
    friend class NativePaintScope;

    void enter(std::uintptr_t hwnd, std::uint32_t tid) noexcept {
        enters_.fetch_add(1, std::memory_order_relaxed);
        if (depth_.fetch_add(1, std::memory_order_acq_rel) == 0) {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            startMs_.store(static_cast<std::uint64_t>(now), std::memory_order_relaxed);
            hwnd_.store(hwnd, std::memory_order_relaxed);
            tid_.store(tid, std::memory_order_relaxed);
        }
    }

    void leave() noexcept {
        exits_.fetch_add(1, std::memory_order_relaxed);
        depth_.fetch_sub(1, std::memory_order_release);
    }

    std::atomic<std::uint64_t> enters_{};
    std::atomic<std::uint64_t> exits_{};
    std::atomic<std::uint32_t> depth_{};
    std::atomic<std::uint64_t> startMs_{};
    std::atomic<std::uintptr_t> hwnd_{};
    std::atomic<std::uint32_t> tid_{};
};

class NativePaintScope {
public:
    NativePaintScope(NativePaintCounter& counter, std::uintptr_t hwnd,
                     std::uint32_t tid) noexcept : counter_(counter) {
        counter_.enter(hwnd, tid);
    }
    ~NativePaintScope() noexcept { counter_.leave(); }
    NativePaintScope(const NativePaintScope&) = delete;
    NativePaintScope& operator=(const NativePaintScope&) = delete;
    NativePaintScope(NativePaintScope&&) = delete;
    NativePaintScope& operator=(NativePaintScope&&) = delete;

private:
    NativePaintCounter& counter_;
};

inline NativePaintCounter overlayPaint;
inline NativePaintCounter syncWindowUpdate;

} // namespace veyra::diagnostics
