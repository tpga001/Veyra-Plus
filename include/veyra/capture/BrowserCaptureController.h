#pragma once
#include "veyra/engine/EngineController.h"
#include "veyra/capture/BrowserTelemetry.h"
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <memory>
#include <functional>

namespace veyra::capture {
// UI-thread owner of the browser attachment and its loopback metadata endpoint.
// The application's existing engine must outlive this object. No enhancement
// algorithm, frame queue or audio path is duplicated here.
class BrowserCaptureController final : public QObject {
    Q_OBJECT
public:
    explicit BrowserCaptureController(engine::EngineController&, QObject* parent=nullptr);
    ~BrowserCaptureController() override;
    bool startListening();
    bool listenerRunning() const;
    // UI-thread configuration admission before a source is stopped. The
    // callback may persist/validate settings, but must not start another source.
    void setEnableAdmission(std::function<bool(bool)> admission);
    void setEnabled(bool enabled);
    void setEnabledFromUser(bool enabled);
    bool enabled() const;
    bool active() const;
    QString status() const;
    QVariantMap diagnostics() const;
    BrowserTelemetryView telemetry() const;
    uint64_t previewSessionId() const;
    HWND sourceWindow() const; // native selection identity, no browser contents
    void setMonitorWindow(HWND);
    void setRtxSourceSized(bool enabled);
    void configure(engine::EnhancementSettings, engine::PresentationSettings);
    void configure(engine::PlayerOptions, engine::PresentationSettings);
    static engine::EnhancementSettings defaultSettings();
signals:
    void changed();
    void enabledChanged(bool enabled);
    // Explicit UI/extension command, including a repeated OFF after a failure.
    void userEnabledRequested(bool enabled);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace veyra::capture
