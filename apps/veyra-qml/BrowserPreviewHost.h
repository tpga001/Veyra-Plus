#pragma once
#include <QObject>
#include <memory>

class QQuickWindow;
namespace veyra::ui {
class QmlPlayerBridge;

// Native side output of the existing browser enhancement graph. This target is
// independent of the ordinary player's primary HWND and never opens a source.
class BrowserPreviewHost final : public QObject {
public:
    BrowserPreviewHost(QQuickWindow&, QmlPlayerBridge&);
    ~BrowserPreviewHost() override;
    // Keep the HWND alive until EngineController::shutdown has joined the worker.
    void detach();
    void destroy();
private:
    void sync();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace veyra::ui
