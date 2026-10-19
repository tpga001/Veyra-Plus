#include "veyra/ui/OnlineSubtitleService.h"
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
struct Fixture {
    MockServer http;
    QString profile;
    QString media;
    std::unique_ptr<OnlineSubtitleService> service;
    int changed = 0;
    int ready = 0;
    explicit Fixture(const QString& root, const QString& name, int timeoutMs = 1500)
        : profile(root + QLatin1Char('/') + name), media(root + QStringLiteral("/synthetic.mp4")) {
        OnlineSubtitleService::TestOptions options;
        options.apiBase = http.base(); options.allowLoopbackHttp = true; options.timeoutMs = timeoutMs;
        service = std::make_unique<OnlineSubtitleService>(profile, options);
        QObject::connect(service.get(), &OnlineSubtitleService::changed, service.get(), [this] { ++changed; });
        QObject::connect(service.get(), &OnlineSubtitleService::subtitleReady, service.get(), [this] { ++ready; });
        http.route = [this](const Request& request) {
            const auto path = request.target.path();
            if (path == QLatin1String("/v1/sub/search")) return Reply{200, "application/json", searchResult()};
            if (path == QLatin1String("/v1/sub/detail")) return Reply{200, "application/json", detailResult(http.fileUrl())};
            return Reply{200, "application/x-subrip", srt};
        };
        expect(http.listening, "mock HTTP server listens only on loopback");
        service->setMedia(media);
        expect(service->configureToken(token), "configure synthetic test token");
        service->setQuery(QString::fromUtf8("星际穿越 2014"));
    }
    bool done(int requestCount) {
        return spinUntil([&] { return http.requests.size() >= requestCount && !service->busy(); });
    }
    void search() {
        service->search();
        expect(done(1), "mock search completes without blocking event loop");
        expect(service->error().isEmpty(), "mock search succeeds");
        expect(service->results().size() == 1, "mock search exposes one result");
    }
    void detail() {
        service->chooseResult(0);
        expect(done(2), "mock detail completes");
        expect(service->error().isEmpty(), "mock detail succeeds");
        expect(service->files().size() == 1, "mock detail exposes downloadable subtitle");
    }
};
void successAndCredentialTests(const QString& root) {
    Fixture fixture(root, QStringLiteral("success"));
    auto& service = *fixture.service;
    expect(service.available(), "existing local media permits online subtitle search");
    expect(service.configured(), "test token is configured");
    drainFor(30);
    expect(fixture.http.requests.isEmpty(), "setMedia and configureToken never search automatically");
    fixture.search();
    if (fixture.http.requests.size() >= 1) {
        const auto& request = fixture.http.requests[0];
        expect(request.method == "GET", "search is a GET request");
        expect(request.headers.value("authorization") == "Bearer " + token.toLatin1(), "API search authenticates with header");
        expect(!request.target.toEncoded().contains(token.toLatin1()), "credential is absent from API URL");
        expect(QUrlQuery(request.target).queryItemValue(QStringLiteral("cnt")) == QLatin1String("15"), "search result count stays bounded to 15");
        expect(QUrlQuery(request.target).queryItemValue(QStringLiteral("no_muxer")) == QLatin1String("1"), "search strips muxer noise through documented provider option");
        expect(QUrlQuery(request.target).queryItemValue(QStringLiteral("q")) == QString::fromUtf8("星际穿越 2014"), "Chinese query is encoded correctly");
        expect(!request.target.toEncoded().contains("synthetic.mp4"), "query does not silently transmit media path or filename");
    }
    fixture.detail();
    if (fixture.http.requests.size() >= 2) {
        const auto& request = fixture.http.requests[1];
        expect(QUrlQuery(request.target).queryItemValue(QStringLiteral("id")) == QLatin1String("123456"), "detail uses selected result ID");
        expect(request.headers.value("authorization") == "Bearer " + token.toLatin1(), "detail authenticates only against API");
    }
    service.downloadFile(0);
    expect(fixture.done(3), "mock download and subtitle parser complete");
    expect(service.error().isEmpty(), "valid SRT download succeeds");
    expect(fixture.ready == 1, "successful download publishes exactly one subtitleReady signal");
    if (fixture.http.requests.size() >= 3) {
        const auto& request = fixture.http.requests[2];
        expect(!request.headers.contains("authorization"), "download never receives ASSRT bearer credential");
        expect(!request.headers.contains("cookie"), "download never receives API cookies");
        expect(!request.target.toEncoded().contains(token.toLatin1()), "download URL contains no token");
    }
    auto track = service.takeReadyTrack();
    expect(track.has_value() && track->cues.size() == 2, "downloaded SRT is actually parsed into two cues");
    if (track && track->cues.size() == 2) {
        expect(track->cues[0].text == L"你好，世界。", "Chinese subtitle text survives UTF-8 download");
        expect(track->cues[0].begin == 1.0 && track->cues[0].end == 3.5, "downloaded timing survives parsing");
        expect(track->cues[1].text.find(L"第二行字幕") != std::wstring::npos, "second Chinese cue survives parsing");
        expect(track->cues[1].text.find(L"English line") != std::wstring::npos, "bilingual cue survives parsing");
    }
    expect(!service.takeReadyTrack().has_value(), "ready track is consumed once");
    expect(fixture.changed > 0, "public changed callbacks report transitions");
    bool sawProtectedCredential = false;
    QDirIterator iterator(fixture.profile, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        QFile file(iterator.next());
        if (!file.open(QIODevice::ReadOnly)) { expect(false, "test profile can be inspected"); continue; }
        const auto bytes = file.readAll();
        expect(!bytes.contains(token.toUtf8()), "test profile never stores plaintext token");
        if (bytes.size() > 32 && !file.fileName().endsWith(QStringLiteral(".srt"))) sawProtectedCredential = true;
    }
    expect(sawProtectedCredential, "protected credential is durably stored");
    OnlineSubtitleService::TestOptions options{fixture.http.base(), true, 1500};
    OnlineSubtitleService reloaded(fixture.profile, options);
    expect(reloaded.configured(), "DPAPI-protected token reloads for same user");
    expect(!reloaded.configureToken(QStringLiteral("too-short")), "invalid token is rejected");
    expect(reloaded.configured(), "invalid token does not erase existing valid credential");
    reloaded.clearToken();
    expect(!reloaded.configured(), "clearToken clears in-memory credential");
    OnlineSubtitleService cleared(fixture.profile, options);
    expect(!cleared.configured(), "clearToken persists across restart");
}
void errorTests(const QString& root) {
    const QList<QByteArray> badPayloads = {
        "{not-json", "<html>upstream error</html>", "{}", "{\"status\":0,\"sub\":{\"subs\":\"bad\"}}",
        "{\"status\":30900,\"errmsg\":\"quota exhausted\"}"
    };
    int index = 0;
    for (const auto& payload : badPayloads) {
        Fixture fixture(root, QStringLiteral("bad-search-%1").arg(index++));
        fixture.http.route = [payload](const Request&) { return Reply{200, "application/json", payload}; };
        fixture.service->search();
        expect(fixture.done(1), "malformed search or provider error completes");
        expect(!fixture.service->error().isEmpty(), "malformed search or provider status produces visible error");
        expect(fixture.service->results().isEmpty() && fixture.ready == 0, "malformed search never becomes result or subtitle");
    }
    {
        Fixture fixture(root, QStringLiteral("http-error"));
        fixture.http.route = [](const Request&) { return Reply{503, "application/json", "{\"status\":30000}"}; };
        fixture.service->search();
        expect(fixture.done(1) && !fixture.service->error().isEmpty(), "HTTP failure produces visible error");
    }
    {
        Fixture fixture(root, QStringLiteral("large-json"));
        fixture.http.route = [](const Request&) { return Reply{200, "application/json", searchResult(), 0, 64LL * 1024 * 1024}; };
        fixture.service->search();
        expect(fixture.done(1) && !fixture.service->error().isEmpty(), "oversized API response is bounded and rejected");
        expect(fixture.service->results().isEmpty(), "oversized API response is never parsed as success");
    }
    {
        Fixture fixture(root, QStringLiteral("timeout"), 150);
        fixture.http.route = [](const Request&) { return Reply{200, "application/json", searchResult(), 600}; };
        fixture.service->search();
        expect(fixture.done(1) && !fixture.service->error().isEmpty(), "slow API request times out");
        drainFor(650);
        expect(fixture.service->results().isEmpty(), "response after timeout never overwrites error state");
    }
    {
        Fixture fixture(root, QStringLiteral("unconfigured"));
        fixture.service->clearToken(); fixture.service->search(); drainFor(30);
        expect(!fixture.service->busy() && !fixture.service->error().isEmpty(), "unconfigured search fails without hanging");
        expect(fixture.http.requests.isEmpty(), "unconfigured search does not contact provider");
    }
}
void metadataSizeAndApiErrorTests(const QString& root) {
    {
        Fixture fixture(root, QStringLiteral("http400-token-status"));
        fixture.http.route = [](const Request&) {
            return Reply{400, "application/json", "{\"status\":20001,\"errmsg\":\"invalid token\"}"};
        };
        fixture.service->search();
        expect(fixture.done(1), "HTTP400 ASSRT error response completes");
        expect(fixture.service->error().contains(QStringLiteral("Token")), "HTTP400 status20001 maps to actionable credential error");
        expect(fixture.service->results().isEmpty() && fixture.ready == 0, "HTTP400 token rejection never exposes results");
    }
    {
        Fixture fixture(root, QStringLiteral("metadata-file-sizes"));
        fixture.http.route = [&fixture](const Request& request) {
            if (request.target.path() == QLatin1String("/v1/sub/search")) return Reply{200, "application/json", searchResult()};
            QJsonArray entries;
            entries.append(QJsonObject{{"f", "numeric.srt"}, {"s", 1234}, {"url", fixture.http.fileUrl()}});
            entries.append(QJsonObject{{"f", "readable.srt"}, {"s", "52KB"}, {"url", fixture.http.fileUrl()}});
            entries.append(QJsonObject{{"f", "boundary.srt"}, {"s", 4 * 1024 * 1024}, {"url", fixture.http.fileUrl()}});
            entries.append(QJsonObject{{"f", "oversized-numeric.srt"}, {"s", 4 * 1024 * 1024 + 1}, {"url", fixture.http.fileUrl()}});
            entries.append(QJsonObject{{"f", "oversized-readable.srt"}, {"s", "5MB"}, {"url", fixture.http.fileUrl()}});
            const QJsonObject row{{"id", 123456}, {"filelist", entries}};
            const QJsonObject payload{{"subs", QJsonArray{row}}};
            return Reply{200, "application/json", json(QJsonObject{{"status", 0}, {"sub", payload}})};
        };
        fixture.search(); fixture.service->chooseResult(0);
        expect(fixture.done(2) && fixture.service->error().isEmpty(), "mixed numeric and readable file-size metadata loads");
        const auto files = fixture.service->files();
        expect(files.size() == 5, "all metadata files remain visible including disabled oversized files");
        if (files.size() == 5) {
            expect(files[0].toMap().value(QStringLiteral("size")).toLongLong() == 1234, "numeric byte size is preserved");
            expect(files[1].toMap().value(QStringLiteral("size")).toLongLong() == 52 * 1024, "52KB is normalized to 53248 bytes");
            expect(files[2].toMap().value(QStringLiteral("size")).toLongLong() == 4 * 1024 * 1024, "4MiB boundary is preserved as bytes");
            expect(files[0].toMap().value(QStringLiteral("supported")).toBool(), "small numeric text subtitle is selectable");
            expect(files[1].toMap().value(QStringLiteral("supported")).toBool(), "human-readable small text subtitle is selectable");
            expect(files[2].toMap().value(QStringLiteral("supported")).toBool(), "exact 4MiB metadata boundary is selectable");
            expect(!files[3].toMap().value(QStringLiteral("supported")).toBool(), "numeric over-4MiB subtitle is disabled before download");
            expect(!files[4].toMap().value(QStringLiteral("supported")).toBool(), "human-readable over-4MiB subtitle is disabled before download");
            fixture.service->downloadFile(4); drainFor(30);
            expect(!fixture.service->busy() && !fixture.service->error().isEmpty(), "disabled oversized result cannot be forced through public download method");
            expect(fixture.http.requests.size() == 2 && fixture.ready == 0, "metadata size rejection performs no download request");
        }
    }
}
void downloadRejectionTests(const QString& root) {
    struct BadFile { QString name; QByteArray body; QByteArray type; qint64 length = -1; };
    const QList<BadFile> badFiles = {
        {QStringLiteral("fixture.srt"), "<html>captcha or server error</html>", "text/html"},
        {QStringLiteral("fixture.srt"), "not a subtitle at all", "text/plain"},
        {QStringLiteral("fixture.zip"), QByteArray("PK\003\004fake", 8), "application/zip"},
        {QStringLiteral("fixture.exe"), "MZfixture-not-executable", "application/octet-stream"},
        {QStringLiteral("fixture.srt"), srt, "application/x-subrip", 64LL * 1024 * 1024}
    };
    int index = 0;
    for (const auto& bad : badFiles) {
        Fixture fixture(root, QStringLiteral("bad-download-%1").arg(index++));
        fixture.http.route = [&fixture, bad](const Request& request) {
            if (request.target.path() == QLatin1String("/v1/sub/search")) return Reply{200, "application/json", searchResult()};
            if (request.target.path() == QLatin1String("/v1/sub/detail")) return Reply{200, "application/json", detailResult(fixture.http.fileUrl(), bad.name)};
            return Reply{200, bad.type, bad.body, 0, bad.length};
        };
        fixture.search(); fixture.service->chooseResult(0);
        expect(fixture.done(2), "unsupported file detail completes");
        if (!fixture.service->files().isEmpty()) {
            fixture.service->downloadFile(0);
            expect(spinUntil([&] { return !fixture.service->busy(); }), "unsupported download completes");
        }
        expect(fixture.ready == 0 && !fixture.service->takeReadyTrack(), "unsupported or malformed download never publishes subtitle");
        expect(!fixture.service->error().isEmpty() || fixture.service->files().isEmpty(), "rejected subtitle is explained or omitted");
    }
    {
        Fixture fixture(root, QStringLiteral("foreign-download"));
        fixture.http.route = [](const Request& request) {
            if (request.target.path() == QLatin1String("/v1/sub/search")) return Reply{200, "application/json", searchResult()};
            return Reply{200, "application/json", detailResult(QStringLiteral("http://127.0.0.2:9/forbidden.srt"))};
        };
        fixture.search(); fixture.service->chooseResult(0); expect(fixture.done(2), "foreign download detail completes");
        if (!fixture.service->files().isEmpty()) { fixture.service->downloadFile(0); drainFor(50); }
        expect(!fixture.service->busy() && fixture.ready == 0, "foreign loopback destination is rejected");
        expect(!fixture.service->error().isEmpty() || fixture.service->files().isEmpty(), "foreign destination is never silently accepted");
    }
}
void encodingTests(const QString& root) {
    const QByteArray prefix("1\n00:00:01,000 --> 00:00:02,000\n");
    const QString unicode = QString::fromUtf8(prefix + QByteArray::fromHex("e4bda0e5a5bd") + "\n");
    QByteArray little = QByteArray::fromHex("fffe"), big = QByteArray::fromHex("feff");
    for (const auto character : unicode) {
        const ushort value = character.unicode();
        little.append(char(value & 0xff)); little.append(char(value >> 8));
        big.append(char(value >> 8)); big.append(char(value & 0xff));
    }
    const QList<QByteArray> encoded = {
        QByteArray::fromHex("efbbbf") + unicode.toUtf8(),
        prefix + QByteArray::fromHex("c4e3bac3") + "\n", little, big
    };
    int index = 0;
    for (const auto& bytes : encoded) {
        Fixture fixture(root, QStringLiteral("encoding-%1").arg(index++));
        fixture.http.route = [&fixture, bytes](const Request& request) {
            if (request.target.path() == QLatin1String("/v1/sub/search")) return Reply{200, "application/json", searchResult()};
            if (request.target.path() == QLatin1String("/v1/sub/detail")) return Reply{200, "application/json", detailResult(fixture.http.fileUrl())};
            return Reply{200, "application/octet-stream", bytes};
        };
        fixture.search(); fixture.detail(); fixture.service->downloadFile(0);
        expect(fixture.done(3) && fixture.ready == 1, "UTF-8 BOM, GB18030 or UTF-16 subtitle loads");
        auto track = fixture.service->takeReadyTrack();
        expect(track && track->cues.size() == 1 && track->cues[0].text == L"\u4f60\u597d", "legacy encoding is normalized to exact Chinese cue");
    }
}
void redirectTests(const QString& root) {
    {
        Fixture fixture(root, QStringLiteral("download-redirect"));
        fixture.http.route = [&fixture](const Request& request) {
            if (request.target.path() == QLatin1String("/v1/sub/search")) {
                Reply reply{200, "application/json", searchResult()};
                reply.headers.insert("Set-Cookie", "api_session=synthetic; Path=/"); return reply;
            }
            if (request.target.path() == QLatin1String("/v1/sub/detail")) return Reply{200, "application/json", detailResult(fixture.http.fileUrl(QStringLiteral("/redirect")))};
            if (request.target.path() == QLatin1String("/redirect")) {
                Reply reply{302, "text/plain", {}}; reply.headers.insert("Location", "/final.srt"); return reply;
            }
            return Reply{200, "application/x-subrip", srt};
        };
        fixture.search(); fixture.detail(); fixture.service->downloadFile(0);
        expect(fixture.done(4) && fixture.ready == 1, "same-origin subtitle redirect successfully loads cues");
        expect(fixture.service->takeReadyTrack().has_value(), "redirect delivers a parsed usable track");
        for (qsizetype at = 2; at < fixture.http.requests.size(); ++at) {
            expect(!fixture.http.requests[at].headers.contains("authorization"), "every download redirect omits bearer token");
            expect(!fixture.http.requests[at].headers.contains("cookie"), "API Set-Cookie never leaks into download redirect");
        }
    }
    {
        Fixture fixture(root, QStringLiteral("foreign-redirect"));
        fixture.http.route = [&fixture](const Request& request) {
            if (request.target.path() == QLatin1String("/v1/sub/search")) return Reply{200, "application/json", searchResult()};
            if (request.target.path() == QLatin1String("/v1/sub/detail")) return Reply{200, "application/json", detailResult(fixture.http.fileUrl(QStringLiteral("/redirect")))};
            Reply reply{302, "text/plain", {}}; reply.headers.insert("Location", "http://127.0.0.2:9/forbidden.srt"); return reply;
        };
        fixture.search(); fixture.detail(); fixture.service->downloadFile(0);
        expect(fixture.done(3) && !fixture.service->error().isEmpty(), "download redirect outside test origin is rejected");
        expect(fixture.ready == 0 && !fixture.service->takeReadyTrack(), "foreign redirect never publishes subtitle");
    }
    {
        Fixture fixture(root, QStringLiteral("api-redirect"));
        fixture.http.route = [](const Request&) {
            Reply reply{302, "text/plain", {}}; reply.headers.insert("Location", "/api-other"); return reply;
        };
        fixture.service->search();
        expect(fixture.done(1) && !fixture.service->error().isEmpty(), "API redirect is rejected rather than forwarding credential");
        drainFor(50);
        expect(fixture.http.requests.size() == 1, "rejected API redirect makes no second authenticated request");
    }
    {
        Fixture fixture(root, QStringLiteral("redirect-loop"));
        fixture.http.route = [&fixture](const Request& request) {
            if (request.target.path() == QLatin1String("/v1/sub/search")) return Reply{200, "application/json", searchResult()};
            if (request.target.path() == QLatin1String("/v1/sub/detail")) return Reply{200, "application/json", detailResult(fixture.http.fileUrl(QStringLiteral("/loop")))};
            Reply reply{302, "text/plain", {}}; reply.headers.insert("Location", "/loop"); return reply;
        };
        fixture.search(); fixture.detail(); fixture.service->downloadFile(0);
        expect(spinUntil([&] { return !fixture.service->busy(); }) && !fixture.service->error().isEmpty(), "redirect loop terminates with visible error");
        expect(fixture.http.requests.size() <= 6 && fixture.ready == 0, "redirect loop has at most three hops and never publishes track");
    }
}
void cancellationTests(const QString& root) {
    {
        Fixture fixture(root, QStringLiteral("cancel-search"));
        fixture.http.route = [](const Request&) { return Reply{200, "application/json", searchResult(), 250}; };
        fixture.service->search();
        expect(spinUntil([&] { return fixture.http.requests.size() == 1; }), "delayed search reached fixture server");
        fixture.service->cancel();
        expect(!fixture.service->busy(), "cancel immediately releases busy state");
        drainFor(350);
        expect(fixture.service->results().isEmpty() && fixture.ready == 0, "cancelled search cannot publish stale result");
    }
    {
        Fixture fixture(root, QStringLiteral("source-change-detail"));
        fixture.search();
        fixture.http.route = [&fixture](const Request&) { return Reply{200, "application/json", detailResult(fixture.http.fileUrl()), 250}; };
        fixture.service->chooseResult(0);
        expect(spinUntil([&] { return fixture.http.requests.size() == 2; }), "delayed detail reached fixture server");
        fixture.service->setMedia(root + QStringLiteral("/second.mp4"));
        expect(!fixture.service->busy(), "source change cancels detail request");
        drainFor(350);
        expect(fixture.service->results().isEmpty() && fixture.service->files().isEmpty(), "source change clears old result and prevents stale detail");
        expect(fixture.ready == 0, "source change never publishes old subtitle");
    }
    {
        Fixture fixture(root, QStringLiteral("cancel-download")); fixture.search(); fixture.detail();
        fixture.http.route = [](const Request&) { return Reply{200, "application/x-subrip", srt, 250}; };
        fixture.service->downloadFile(0);
        expect(spinUntil([&] { return fixture.http.requests.size() == 3; }), "delayed download reached fixture server");
        fixture.service->cancel(); drainFor(350);
        expect(!fixture.service->busy() && fixture.ready == 0 && !fixture.service->takeReadyTrack(), "cancelled download cannot publish old subtitle");
    }
    {
        Fixture fixture(root, QStringLiteral("source-change-download")); fixture.search(); fixture.detail();
        fixture.http.route = [](const Request&) { return Reply{200, "application/x-subrip", srt, 250}; };
        fixture.service->downloadFile(0);
        expect(spinUntil([&] { return fixture.http.requests.size() == 3; }), "download reached server before source switch");
        fixture.service->setMedia(root + QStringLiteral("/second.mp4")); drainFor(350);
        expect(!fixture.service->busy() && fixture.ready == 0 && !fixture.service->takeReadyTrack(), "source-switched download never reaches current playback");
    }
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) { std::cerr << "usage: veyra_online_subtitle_tests <new scratch directory>\n"; return 2; }
    const auto root = QFileInfo(app.arguments()[1]).absoluteFilePath();
    if (QFileInfo::exists(root) || !QDir().mkpath(root)) { std::cerr << "scratch directory must be new\n"; return 2; }
    for (const auto& name : {QStringLiteral("synthetic.mp4"), QStringLiteral("second.mp4")}) {
        QFile file(root + QLatin1Char('/') + name);
        if (!file.open(QIODevice::WriteOnly) || file.write("synthetic-media-not-decoded") < 0) return 2;
    }
    successAndCredentialTests(root);
    errorTests(root);
    metadataSizeAndApiErrorTests(root);
    downloadRejectionTests(root);
    encodingTests(root);
    redirectTests(root);
    cancellationTests(root);
    std::cout << "MOCK_ONLY OnlineSubtitleService: " << (assertions - failures) << "/" << assertions << " assertions passed; failures=" << failures << '\n';
    return failures ? 1 : 0;
}