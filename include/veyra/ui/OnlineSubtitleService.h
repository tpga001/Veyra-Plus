#pragma once

#include "veyra/engine/Subtitles.h"
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <memory>
#include <optional>

namespace veyra::ui {

// On-demand ASSRT search/download. No network request is made until the user
// explicitly searches or chooses a result. Credentials never enter QML.
class OnlineSubtitleService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool configured READ configured NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString selectedTitle READ selectedTitle NOTIFY changed)
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)
    Q_PROPERTY(QVariantList files READ files NOTIFY changed)
public:
    explicit OnlineSubtitleService(QString dataDirectory, QObject* parent = nullptr);
    // Native tests only: no environment variable, command-line or QML setter can
    // change production endpoints or permit cleartext requests.
    struct TestOptions {
        QUrl apiBase;
        bool allowLoopbackHttp = false;
        int timeoutMs = 15000;
    };
    OnlineSubtitleService(QString dataDirectory, const TestOptions& options, QObject* parent = nullptr);
    ~OnlineSubtitleService() override;

    bool available() const;
    bool configured() const;
    bool busy() const;
    QString query() const;
    void setQuery(const QString& value);
    QString status() const;
    QString error() const;
    QString selectedTitle() const;
    QVariantList results() const;
    QVariantList files() const;

    void setMedia(const QString& localMedia);
    QString currentMedia() const;
    std::optional<engine::SubtitleTrack> takeReadyTrack();
    Q_INVOKABLE bool configureToken(const QString& token);
    Q_INVOKABLE void clearToken();
    Q_INVOKABLE void search();
    Q_INVOKABLE void chooseResult(int index);
    Q_INVOKABLE void downloadFile(int index);
    Q_INVOKABLE void cancel();
signals:
    void changed();
    void subtitleReady();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace veyra::ui
