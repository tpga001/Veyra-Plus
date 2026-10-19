#pragma once
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

class QWindow;
namespace veyra::ui {
// Diagnostic evidence only: no GPU work, window activation or automatic restart.
// GUI callbacks update bounded memory; an independent thread persists stalls.
class UiResponsivenessMonitor final : public QObject {
    Q_OBJECT
public:
    struct Options {
        // The injected clock must be thread-safe. Production uses steady_clock.
        std::function<qint64()> monotonicMilliseconds;
        bool automaticHeartbeat = true; // false only for CPU tests
    };
    explicit UiResponsivenessMonitor(QString sharedDataDirectory, QObject* parent = nullptr);
    UiResponsivenessMonitor(QString sharedDataDirectory, Options options, QObject* parent = nullptr);
    ~UiResponsivenessMonitor() override;
    void attachWindow(QWindow* window);
    // Fixed allow-list only: show, hide, close, minimize, nav.browser, nav.pro,
    // nav.node, dialog.close, quit. Untrusted/media text is silently discarded.
    void noteAction(const QString& stage);
    // Render-thread safe. Numeric stages 0..7; never waits for a GPU or GUI lock.
    void noteRenderStage(int stage) noexcept;
    // Process-lifetime atomics: safe for direct render callbacks during teardown.
    // UI: 0 event loop, 1 poll, 2 subtitles, 3 capture, 4 geometry, 5 notifications.
    // Render: 1 beforeSync, 2 afterSync, 3 beforeRender, 4 afterRender, 5 swapped.
    static void setUiStage(int stage) noexcept;
    static void setRenderStage(int stage) noexcept;
    QString diagnosticPath() const;
    // C++ test seams, not exposed to QML/IPC/command-line.
    void heartbeatForTest();
    bool pollForTest(unsigned timeoutMilliseconds = 1000);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    void heartbeat();
};
} // namespace veyra::ui
