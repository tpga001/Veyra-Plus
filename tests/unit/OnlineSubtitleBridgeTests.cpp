#include "veyra/ui/OnlineSubtitleService.h"
#include "veyra/ui/QmlPlayerBridge.h"
#include "veyra/engine/EngineController.h"
#include <QApplication>
#include <filesystem>
#include <windows.h>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>

using veyra::ui::OnlineSubtitleService;
namespace {
// Entirely synthetic HTTP responses and credentials: no real provider is queried.
const QString token(32, QLatin1Char('X'));
int assertions = 0;
int failures = 0;
void expect(bool value, const char* message) {
    ++assertions;
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
bool spinUntil(const std::function<bool()>& predicate, int timeoutMs = 4000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}
void drainFor(int ms) {
    QElapsedTimer timer; timer.start();
    spinUntil([&] { return timer.elapsed() >= ms; }, ms + 1000);
}
struct Request {
    QByteArray method;
    QUrl target;
    QMap<QByteArray, QByteArray> headers;
};
struct Reply {
    int status = 200;
    QByteArray type = "application/json";
    QByteArray body;
    int delayMs = 0;
    qint64 declaredLength = -1;
    QMap<QByteArray, QByteArray> headers;
};
QByteArray json(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QByteArray searchResult() {
    const QJsonObject row{
        {"id", 123456}, {"native_name", QString::fromUtf8("合成测试中文字幕")},
        {"videoname", "Synthetic.Movie.2014.1080p"}, {"subtype", "Subrip(srt)"},
        {"lang", QJsonObject{{"desc", QString::fromUtf8("简体中文")}}}
    };
    const QJsonObject payload{{"subs", QJsonArray{row}}};
    return json(QJsonObject{{"status", 0}, {"sub", payload}});
}
QByteArray detailResult(const QString& url, const QString& filename = QStringLiteral("synthetic.zh.srt")) {
    const QJsonObject file{{"f", filename}, {"s", "1KB"}, {"url", url}};
    const QJsonObject row{
        {"id", 123456}, {"native_name", QString::fromUtf8("合成测试中文字幕")},
        {"filelist", QJsonArray{file}}
    };
    const QJsonObject payload{{"subs", QJsonArray{row}}};
    return json(QJsonObject{{"status", 0}, {"sub", payload}});
}
const QByteArray srt = QString::fromUtf8(
    "1\n00:00:01,000 --> 00:00:03,500\n你好，世界。\n\n"
    "2\n00:00:04,000 --> 00:00:06,000\n第二行字幕\nEnglish line\n").toUtf8();
class MockServer final : public QObject {
public:
    QTcpServer server;
    QList<Request> requests;
    std::function<Reply(const Request&)> route;
    bool listening = false;
    MockServer() {
        listening = server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                auto bytes = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes, handled] {
                    *bytes += socket->readAll();
                    if (*handled || !bytes->contains("\r\n\r\n")) return;
                    *handled = true;
                    const auto lines = bytes->left(bytes->indexOf("\r\n\r\n")).split('\n');
                    const auto first = lines.value(0).trimmed().split(' ');
                    Request request; request.method = first.value(0);
                    request.target = QUrl::fromEncoded(first.value(1));
                    for (qsizetype i = 1; i < lines.size(); ++i) {
                        const auto colon = lines[i].indexOf(':');
                        if (colon > 0) request.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
                    }
                    requests.push_back(request);
                    Reply reply = route ? route(request) : Reply{404, "text/plain", "missing fixture"};
                    QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(reply.delayMs, socket, [guard, reply] {
                        if (!guard || guard->state() != QAbstractSocket::ConnectedState) return;
                        const qint64 length = reply.declaredLength < 0 ? reply.body.size() : reply.declaredLength;
                        QByteArray wire = "HTTP/1.1 " + QByteArray::number(reply.status) + " Fixture\r\nContent-Type: " + reply.type
                            + "\r\nContent-Length: " + QByteArray::number(length) + "\r\nConnection: close\r\n";
                        for (auto it = reply.headers.cbegin(); it != reply.headers.cend(); ++it) wire += it.key() + ": " + it.value() + "\r\n";
                        guard->write(wire + "\r\n" + reply.body);
                        guard->disconnectFromHost();
                    });
                });
            }
        });
    }
    QUrl base() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())); }
    QString fileUrl(const QString& path = QStringLiteral("/synthetic.srt")) const { return base().resolved(QUrl(path)).toString(); }
};

} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().size() != 3) {
        std::cerr << "usage: veyra_online_subtitle_bridge_tests <new scratch directory> <synthetic fixture video>\n"; return 2;
    }
    const auto root = QFileInfo(app.arguments()[1]).absoluteFilePath();
    const auto media = QFileInfo(app.arguments()[2]).absoluteFilePath();
    if (QFileInfo::exists(root) || !QDir().mkpath(root) || !QFileInfo(media).isFile()) {
        std::cerr << "requires a new private scratch directory and an existing synthetic video\n"; return 2;
    }
    const auto second = root + QStringLiteral("/second-synthetic.mkv");
    if (!QFile::copy(media, second)) { std::cerr << "cannot copy synthetic fixture\n"; return 2; }
    MockServer http;
    expect(http.listening, "bridge fixture server listens only on loopback");
    int delayDownloadMs = 0;
    const QByteArray bridgeSubtitle = QString::fromUtf8(
        "1\n00:00:00,000 --> 00:10:00,000\n在线字幕集成测试\n").toUtf8();
    http.route = [&](const Request& request) {
        if (request.target.path() == QLatin1String("/v1/sub/search")) return Reply{200, "application/json", searchResult()};
        if (request.target.path() == QLatin1String("/v1/sub/detail")) return Reply{200, "application/json", detailResult(http.fileUrl())};
        return Reply{200, "application/x-subrip", bridgeSubtitle, delayDownloadMs};
    };
    OnlineSubtitleService::TestOptions options{http.base(), true, 1500};
    veyra::engine::EngineController engine;
    engine.setEnhancementPrewarmEnabled(false);
    OnlineSubtitleService* service = nullptr;
    const auto profile = root + QStringLiteral("/profile");
    veyra::ui::QmlPlayerBridge bridge(engine, std::filesystem::path(profile.toStdWString()),
        [&](QString dataDirectory) {
            auto created = std::make_unique<OnlineSubtitleService>(dataDirectory, options);
            service = created.get(); return created;
        });
    expect(service && bridge.onlineSubtitles() == service, "native factory injects actual bridge-owned service");
    if (!service) return 1;
    int ready = 0, tracksChanged = 0;
    QObject::connect(service, &OnlineSubtitleService::subtitleReady, &bridge, [&] { ++ready; });
    QObject::connect(&bridge, &veyra::ui::QmlPlayerBridge::subtitlesChanged, &bridge, [&] { ++tracksChanged; });
    expect(service->configureToken(token), "synthetic credential is stored only in private bridge profile");
    bridge.setNrEnabled(false); bridge.setSrEnabled(false); bridge.setFgEnabled(false); bridge.setBeautyEnabled(false);
    engine.setVolume(0.0f, true);
    // This native test has a hidden valid HWND so the real engine can establish
    // source identity. It does not assert visible pixels or displayed frame rate.
    HWND window = CreateWindowExW(0, L"STATIC", L"Veyra synthetic subtitle bridge test",
        WS_OVERLAPPEDWINDOW, -20000, -20000, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    expect(window != nullptr, "hidden native fixture window is valid");
    if (!window) return 1;
    bridge.attachVideoWindow(reinterpret_cast<qulonglong>(window));
    bridge.openPath(media);
    const bool running = spinUntil([&] { return bridge.sourceKind() == QLatin1String("file"); }, 15000);
    expect(running, "synthetic file establishes real local-video source identity");
    if (!running) {
        bridge.stopPlayback(); engine.stop(); DestroyWindow(window);
        std::cout << "MOCK_ONLY bridge: cannot establish local source; no subtitle insertion acceptance\n"; return 1;
    }
    engine.pause(true);
    expect(service->currentMedia() == media && service->available(), "bridge sets service media to current absolute local path");
    const auto prepareDownload = [&] {
        const auto before = http.requests.size();
        service->setQuery(QString::fromUtf8("合成字幕测试")); service->search();
        expect(spinUntil([&] { return http.requests.size() >= before + 1 && !service->busy(); }), "bridge-owned service completes mock search");
        expect(service->results().size() == 1, "bridge-owned service publishes actual search result");
        service->chooseResult(0);
        expect(spinUntil([&] { return http.requests.size() >= before + 2 && !service->busy(); }), "bridge-owned service completes mock detail");
        expect(service->files().size() == 1, "bridge-owned service offers actual subtitle file");
        return http.requests.size();
    };
    prepareDownload(); service->downloadFile(0);
    expect(spinUntil([&] { return ready == 1 && !service->busy(); }), "mock network download emits real service subtitleReady");
    expect(spinUntil([&] {
        const auto tracks = bridge.subtitleTracks();
        return !tracks.isEmpty() && tracks[0].toMap().value(QStringLiteral("label")).toString().contains(QStringLiteral("ASSRT"));
    }), "subtitleReady inserts downloaded track into real bridge model");
    const auto tracks = bridge.subtitleTracks();
    if (!tracks.isEmpty()) {
        const auto first = tracks[0].toMap();
        expect(first.value(QStringLiteral("usable")).toBool(), "bridge inserted parsed usable subtitle track");
        expect(first.value(QStringLiteral("cues")).toInt() == 1, "bridge inserted one actual downloaded cue");
    }
    expect(bridge.subtitlePrimary() == 0, "downloaded track becomes primary selection");
    expect(bridge.subtitleStatus().contains(QStringLiteral("ASSRT")), "bridge exposes loaded subtitle status");
    expect(!service->takeReadyTrack(), "bridge consumed the actual service ready track");
    expect(tracksChanged > 0, "real subtitlesChanged signal notified the UI model");
    expect(spinUntil([&] { return bridge.subtitleText().contains(QString::fromUtf8("在线字幕集成测试")); }),
        "downloaded Chinese cue reaches real bridge subtitle text output");
    QFile credential(profile + QStringLiteral("/online-subtitle-token.v1.bin"));
    expect(credential.open(QIODevice::ReadOnly), "bridge credential is stored under private data directory");
    if (credential.isOpen()) expect(!credential.readAll().contains(token.toUtf8()), "bridge private credential is encrypted rather than plaintext");

    prepareDownload(); delayDownloadMs = 250;
    const auto beforeStop = http.requests.size(); service->downloadFile(0);
    expect(spinUntil([&] { return http.requests.size() > beforeStop; }), "download is outstanding before stop");
    bridge.stopPlayback();
    expect(service->currentMedia().isEmpty() && !service->available() && !service->busy(), "stop invalidates online subtitle media and outstanding request");
    drainFor(350);
    expect(ready == 1 && !service->takeReadyTrack(), "stopped playback never receives delayed download completion");

    bridge.openPath(media);
    expect(spinUntil([&] { return bridge.sourceKind() == QLatin1String("file"); }, 15000), "synthetic source reopens for source-switch isolation");
    engine.pause(true);
    prepareDownload();
    const auto beforeSwitch = http.requests.size(); service->downloadFile(0);
    expect(spinUntil([&] { return http.requests.size() > beforeSwitch; }), "download is outstanding before source switch");
    bridge.openPath(second);
    expect(service->currentMedia() == second, "source switch updates actual service source identity");
    drainFor(350);
    expect(ready == 1 && !service->takeReadyTrack(), "source-switched playback never receives old downloaded track");
    const auto switched = bridge.subtitleTracks();
    bool hasOldOnlineTrack = false;
    for (const auto& row : switched)
        hasOldOnlineTrack |= row.toMap().value(QStringLiteral("label")).toString().contains(QStringLiteral("ASSRT"));
    expect(!hasOldOnlineTrack, "new source model contains no old online track");
    bridge.stopPlayback(); engine.stop();
    spinUntil([&] { return !engine.snapshot().running; }, 5000);
    DestroyWindow(window);
    std::cout << "MOCK_ONLY OnlineSubtitleBridge: " << (assertions - failures) << "/" << assertions
              << " assertions passed; failures=" << failures << "; no visible-pixel or live-provider claim\n";
    return failures ? 1 : 0;
}
