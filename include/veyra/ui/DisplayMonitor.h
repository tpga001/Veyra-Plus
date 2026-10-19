#pragma once
#include "veyra/diagnostics/DisplayFrameMetrics.h"
#include <QObject>
#include <QVariantMap>
#include <functional>
#include <memory>
#include <string>
namespace veyra::ui {
struct DisplayMonitorSource {
    diagnostics::DisplayTarget target;
    bool available=false, playing=false;
    std::string sourceKind="none", expectedGeneratedFrameType;
};
// UI-thread coordinator only. Rendering never waits on a trace, IPC or UAC.
// The separately owned collector is limited to this process and dies with it.
class DisplayMonitor final : public QObject {
    Q_OBJECT
public:
    explicit DisplayMonitor(QObject* parent=nullptr);
    ~DisplayMonitor() override;
    void setSourceProvider(std::function<DisplayMonitorSource()> provider);
    QVariantMap metrics() const;
    double fps() const;
    bool known() const;
    void authorize();
    std::vector<double> frameDurations() const;
signals:
    void changed();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
