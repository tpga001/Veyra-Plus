#pragma once
#include "veyra/diagnostics/DisplayFrameMetrics.h"
#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace veyra::ui {
struct VideoOutputSource {
    diagnostics::DisplayTarget target;
    bool available = false, playing = false, frameGenerationActive = false;
    std::string sourceKind = "none", frameGenerationBackend, expectedGeneratedFrameType;
    uint64_t videoHwnd = 0, sourceHwnd = 0;
};

// Publishes output identity only. SidePanel owns ETW and actual FPS measurement.
// UI ticks perform no filesystem work; a capacity-one mailbox feeds the writer.
class VideoOutputPublisher final : public QObject {
public:
    explicit VideoOutputPublisher(const QString& sharedDataDirectory,
                                  const QString& role = QStringLiteral("browser-resident"),
                                  QObject* parent = nullptr);
    ~VideoOutputPublisher() override;
    void setSourceProvider(std::function<VideoOutputSource()> provider);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace veyra::ui
