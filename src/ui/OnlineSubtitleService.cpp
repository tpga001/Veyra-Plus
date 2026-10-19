#include "veyra/ui/OnlineSubtitleService.h"

#include <windows.h>
#include <dpapi.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <limits>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace veyra::ui {
namespace {
constexpr qsizetype kMaxMetadata = 1024 * 1024;
constexpr qsizetype kMaxSubtitle = 4 * 1024 * 1024;
const QUrl kApiBase(QStringLiteral("https://api.assrt.net/"));

bool validToken(const QByteArray& value) {
    return QRegularExpression(QStringLiteral("^[A-Za-z0-9]{32}$"))
        .match(QString::fromLatin1(value)).hasMatch();
}
QString cleanQuery(const QString& media) {
    QString value = QFileInfo(media).completeBaseName();
    value.replace(QRegularExpression(QStringLiteral("[._]+")), QStringLiteral(" "));
    value.replace(QRegularExpression(QStringLiteral("\\b(?:480[pi]|576[pi]|720[pi]|1080[pi]|2160[pi]|4320[pi]|x264|x265|h264|h265|hevc|avc|bluray|brrip|webrip|web[- ]?dl|hdtv|aac|dts|ddp|10bit)\\b"), QRegularExpression::CaseInsensitiveOption), QStringLiteral(" "));
    return value.simplified().left(200);
}
bool loopback(const QString& host) {
    return host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") || host == QLatin1String("::1");
}
bool assrtHost(const QString& host) {
    const auto value = host.toLower();
    return value == QLatin1String("assrt.net") || value.endsWith(QLatin1String(".assrt.net"));
}
QByteArray protectToken(const QByteArray& plain, bool encrypt) {
    DATA_BLOB input{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    DATA_BLOB output{};
    // Fixed application entropy is namespace separation, not another secret.
    QByteArray entropyBytes("Veyra online subtitle token v1");
    DATA_BLOB entropy{DWORD(entropyBytes.size()), reinterpret_cast<BYTE*>(entropyBytes.data())};
    const BOOL ok = encrypt
        ? CryptProtectData(&input, L"Veyra ASSRT token", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
        : CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok) return {};
    QByteArray result(reinterpret_cast<const char*>(output.pbData), qsizetype(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
QString decodeBytes(const QByteArray& bytes, UINT codepage) {
    const int count = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, bytes.constData(), int(bytes.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring wide(size_t(count), L'\0');
    if (MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, bytes.constData(), int(bytes.size()), wide.data(), count) != count) return {};
    return QString::fromStdWString(wide);
}
QByteArray normalizeSubtitle(QByteArray bytes) {
    if (bytes.isEmpty() || bytes.size() > kMaxSubtitle) return {};
    QString text;
    if (bytes.startsWith("\xFF\xFE") || bytes.startsWith("\xFE\xFF")) {
        if ((bytes.size() - 2) % 2) return {};
        const bool little = bytes.startsWith("\xFF\xFE");
        text.reserve((bytes.size() - 2) / 2);
        for (qsizetype at = 2; at + 1 < bytes.size(); at += 2) {
            const auto a = static_cast<unsigned char>(bytes[at]);
            const auto b = static_cast<unsigned char>(bytes[at + 1]);
            text.append(QChar(little ? ushort(a | (b << 8)) : ushort((a << 8) | b)));
        }
    } else {
        if (bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);
        text = decodeBytes(bytes, CP_UTF8);
        if (text.isEmpty()) text = decodeBytes(bytes, 54936); // Windows GB18030 also accepts common GBK subtitles.
    }
    if (text.isEmpty() || text.contains(QChar(u'\0'))) return {};
    const auto utf8 = text.toUtf8();
    return utf8.size() <= kMaxSubtitle ? utf8 : QByteArray{};
}
std::optional<qint64> declaredFileSize(const QJsonValue& value) {
    double bytes = 0;
    if (value.isDouble()) bytes = value.toDouble(-1);
    else {
        static const QRegularExpression pattern(QStringLiteral("^([0-9]+(?:\\.[0-9]+)?)\\s*(B|KB|KIB|MB|MIB|GB|GIB)?$"), QRegularExpression::CaseInsensitiveOption);
        const auto match = pattern.match(value.toString().trimmed());
        if (!match.hasMatch()) return {};
        bytes = match.captured(1).toDouble();
        const auto unit = match.captured(2).toUpper();
        if (unit == QLatin1String("KB") || unit == QLatin1String("KIB")) bytes *= 1024.;
        else if (unit == QLatin1String("MB") || unit == QLatin1String("MIB")) bytes *= 1024. * 1024.;
        else if (unit == QLatin1String("GB") || unit == QLatin1String("GIB")) bytes *= 1024. * 1024. * 1024.;
    }
    if (!std::isfinite(bytes) || bytes < 0 || bytes >= double(std::numeric_limits<qint64>::max())) return {};
    return qint64(bytes);
}
QString apiError(int status) {
    switch (status) {
    case 1: case 20001: return OnlineSubtitleService::tr("ASSRT Token 无效，请重新配置本人 Token");
    case 101: return OnlineSubtitleService::tr("搜索词太短，请输入至少 3 个字符");
    case 20900: return OnlineSubtitleService::tr("该字幕已不存在，请重新搜索");
    case 30900: return OnlineSubtitleService::tr("字幕服务请求过于频繁，请稍后重试");
    default: return OnlineSubtitleService::tr("字幕服务返回错误（%1），请稍后重试").arg(status);
    }
}
}

struct OnlineSubtitleService::Impl {
    OnlineSubtitleService* owner;
    QString dataDirectory, media, query, status, error, selectedTitle;
    QByteArray token;
    QVariantList results, files;
    struct Download { QUrl url; QString extension, displayName; };
    std::vector<Download> downloads;
    QNetworkAccessManager manager;
    QPointer<QNetworkReply> reply;
    QTimer parsePoll;
    QUrl apiBase;
    bool testLoopback = false, busy = false;
    int timeoutMs = 15000;
    quint64 generation = 0;
    std::optional<engine::SubtitleTrack> readyTrack;
    struct ParseJob { quint64 generation; QByteArray bytes; QString extension, displayName; };
    struct Parsed { quint64 generation; std::optional<engine::SubtitleTrack> track; QString error; };
    std::mutex parseMutex;
    std::condition_variable_any parseWake;
    std::optional<ParseJob> pending;
    std::optional<Parsed> parsed;
    // Declared last: the worker must stop before any state it touches dies.
    std::jthread parser;

    Impl(OnlineSubtitleService* q, QString directory, const TestOptions& options)
        : owner(q), dataDirectory(QDir(directory).absolutePath()), manager(q),
          apiBase(options.apiBase.isEmpty() ? kApiBase : options.apiBase),
          testLoopback(options.allowLoopbackHttp && loopback(apiBase.host())),
          timeoutMs(std::clamp(options.timeoutMs, 100, 60000)) {
        // The test constructor only permits an explicit loopback origin. Other
        // custom endpoints are never allowed to receive the user's credential.
        if (!testLoopback) apiBase = kApiBase;
        apiBase.setQuery(QString{}); apiBase.setFragment(QString{});
        QString path = apiBase.path(); if (!path.endsWith(QLatin1Char('/'))) path += QLatin1Char('/');
        apiBase.setPath(path);
        QFile saved(tokenPath());
        if (saved.open(QIODevice::ReadOnly) && saved.size() > 0 && saved.size() <= 65536) {
            auto plain = protectToken(saved.readAll(), false);
            if (validToken(plain)) token = plain;
            else error = OnlineSubtitleService::tr("保存的 Token 无法解密，请重新配置");
            if (!plain.isEmpty()) SecureZeroMemory(plain.data(), size_t(plain.size()));
        }
        status = OnlineSubtitleService::tr("请先打开本地视频");
        parsePoll.setInterval(100);
        QObject::connect(&parsePoll, &QTimer::timeout, q, [this] { pollParse(); });
        parsePoll.start();
        parser = std::jthread([this](std::stop_token shutdown) { parseLoop(shutdown); });
    }
    ~Impl() {
        if (reply) { QObject::disconnect(reply, nullptr, owner, nullptr); reply->abort(); }
        parser.request_stop(); parseWake.notify_all();
        if (parser.joinable()) parser.join();
        if (!token.isEmpty()) SecureZeroMemory(token.data(), size_t(token.size()));
    }
    QString tokenPath() const { return QDir(dataDirectory).filePath(QStringLiteral("online-subtitle-token.v1.bin")); }
    QString cacheDirectory() const { return QDir(dataDirectory).filePath(QStringLiteral("online-subtitles")); }
    void notify() { emit owner->changed(); }
    void fail(const QString& message) { busy = false; error = message; status = OnlineSubtitleService::tr("操作未完成"); notify(); }
    void invalidate() {
        ++generation;
        if (reply) { auto* old = reply.data(); reply.clear(); QObject::disconnect(old, nullptr, owner, nullptr); old->abort(); old->deleteLater(); }
        { std::lock_guard lock(parseMutex); pending.reset(); parsed.reset(); }
        readyTrack.reset(); busy = false; error.clear();
    }
    bool admitted() {
        if (media.isEmpty()) { fail(OnlineSubtitleService::tr("在线字幕仅适用于已打开的本地视频")); return false; }
        if (!validToken(token)) { fail(OnlineSubtitleService::tr("请先配置本人的 ASSRT Token")); return false; }
        return true;
    }
    QUrl endpoint(const QString& path, const QUrlQuery& query) const {
        auto url = apiBase.resolved(QUrl(path)); url.setQuery(query); return url;
    }
    bool safeDownload(QUrl& url) const {
        if (!url.isValid() || url.isRelative() || !url.userInfo().isEmpty() || !url.fragment().isEmpty()) return false;
        if (testLoopback && loopback(url.host()) && url.host() == apiBase.host() && url.port() == apiBase.port())
            return url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https");
        if (!assrtHost(url.host()) || (url.port() != -1 && url.port() != 80 && url.port() != 443)) return false;
        if (url.scheme() == QLatin1String("http")) { url.setScheme(QStringLiteral("https")); url.setPort(-1); }
        return url.scheme() == QLatin1String("https") && (url.port() == -1 || url.port() == 443);
    }
    using Completion = std::function<void(QByteArray)>;
    void request(const QUrl& url, bool api, qsizetype limit, quint64 id, Completion complete, int redirects = 0) {
        if (id != generation) return;
        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", "Veyra/2.0 OnlineSubtitles");
        request.setRawHeader("Accept", api ? "application/json" : "*/*");
        if (api) request.setRawHeader("Authorization", "Bearer " + token);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        // API cookies are unnecessary for bearer auth and must never propagate
        // to file hosts (including a redirect back to the API's own origin).
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setTransferTimeout(timeoutMs);
        auto* current = manager.get(request); reply = current;
        current->setReadBufferSize(limit + 1);
        auto bytes = std::make_shared<QByteArray>();
        auto read = [current, bytes, limit] {
            const qint64 declared = current->header(QNetworkRequest::ContentLengthHeader).toLongLong();
            if (declared > limit || current->bytesAvailable() > limit - bytes->size()) {
                current->setProperty("veyraTooLarge", true); current->abort(); return;
            }
            *bytes += current->readAll();
        };
        QObject::connect(current, &QNetworkReply::readyRead, owner, read);
        auto* deadline = new QTimer(current); deadline->setSingleShot(true); deadline->start(timeoutMs);
        QObject::connect(deadline, &QTimer::timeout, owner, [current] { current->setProperty("veyraTimeout", true); current->abort(); });
        QObject::connect(current, &QNetworkReply::finished, owner,
            [this, current, deadline, bytes, read, api, limit, id, complete = std::move(complete), redirects]() mutable {
                deadline->stop();
                read();
                if (reply == current) reply.clear();
                current->deleteLater();
                if (id != generation) return;
                if (current->property("veyraTooLarge").toBool()) { fail(OnlineSubtitleService::tr("字幕或服务响应过大，已取消下载（字幕上限 4 MiB）")); return; }
                if (current->property("veyraTimeout").toBool()) { fail(OnlineSubtitleService::tr("连接字幕服务超时，请稍后重试")); return; }
                const int code = current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const auto redirect = current->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
                if (code >= 300 && code < 400) {
                    auto next = current->url().resolved(redirect);
                    if (api || redirects >= 3 || !safeDownload(next)) { fail(OnlineSubtitleService::tr("字幕服务返回了不允许的重定向")); return; }
                    this->request(next, false, limit, id, std::move(complete), redirects + 1); return;
                }
                if (current->error() != QNetworkReply::NoError || code < 200 || code >= 300) {
                    // API errors may use HTTP 400 as well as HTTP 200. Parse
                    // only the bounded status field, never display server text.
                    if (api) {
                        const auto object = QJsonDocument::fromJson(*bytes).object();
                        const auto status = object.value(QStringLiteral("status"));
                        if (status.isDouble() && status.toInt() != 0) { fail(apiError(status.toInt())); return; }
                    }
                    // Never expose request URLs, Authorization or arbitrary server text.
                    if (code == 401 || code == 403) fail(OnlineSubtitleService::tr("字幕服务拒绝访问，请检查本人 Token 或重新选择字幕"));
                    else if (code == 429) fail(OnlineSubtitleService::tr("字幕服务请求过于频繁，请稍后重试"));
                    else fail(OnlineSubtitleService::tr("字幕服务连接失败（HTTP %1），请稍后重试").arg(code));
                    return;
                }
                complete(std::move(*bytes));
            });
    }
    std::optional<QJsonArray> subtitleResponse(const QByteArray& bytes) {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) { fail(OnlineSubtitleService::tr("字幕服务返回了无效数据")); return {}; }
        const auto object = document.object();
        if (!object.value(QStringLiteral("status")).isDouble()) { fail(OnlineSubtitleService::tr("字幕服务缺少状态信息")); return {}; }
        const int code = object.value(QStringLiteral("status")).toInt(-1);
        if (code != 0) { fail(apiError(code)); return {}; }
        const auto subs = object.value(QStringLiteral("sub")).toObject().value(QStringLiteral("subs"));
        if (!subs.isArray()) { fail(OnlineSubtitleService::tr("字幕服务返回了无效字幕列表")); return {}; }
        return subs.toArray();
    }
    void parseLoop(std::stop_token shutdown) {
        while (!shutdown.stop_requested()) {
            std::unique_lock lock(parseMutex);
            if (!parseWake.wait(lock, shutdown, [this] { return pending.has_value(); })) break;
            auto job = std::move(*pending); pending.reset(); lock.unlock();
            Parsed result{job.generation, {}, {}};
            try {
                const auto normalized = normalizeSubtitle(std::move(job.bytes));
                if (normalized.isEmpty()) result.error = OnlineSubtitleService::tr("字幕编码无效或转换后过大，支持 UTF-8、UTF-16 和 GB18030");
                else {
                    const auto digest = QCryptographicHash::hash(normalized, QCryptographicHash::Sha256).toHex();
                    const QString path = QDir(cacheDirectory()).filePath(QStringLiteral("assrt-%1.%2").arg(QString::fromLatin1(digest), job.extension));
                    QSaveFile file(path);
                    if (!QDir().mkpath(cacheDirectory()) || !file.open(QIODevice::WriteOnly) || file.write(normalized) != normalized.size() || !file.commit())
                        result.error = OnlineSubtitleService::tr("无法保存字幕缓存，请检查数据目录权限");
                    else {
                        auto track = engine::loadSubtitleFile(path.toStdWString());
                        if (!track.usable()) result.error = OnlineSubtitleService::tr("下载内容没有有效字幕，未加载；请换一个结果");
                        else { track.name = (job.displayName + QStringLiteral(" · ASSRT")).toStdWString(); result.track = std::move(track); }
                    }
                }
            } catch (...) { result.error = OnlineSubtitleService::tr("字幕解析失败，未更改当前字幕"); }
            lock.lock(); parsed = std::move(result);
        }
    }
    void pollParse() {
        std::optional<Parsed> result;
        { std::lock_guard lock(parseMutex); result = std::move(parsed); parsed.reset(); }
        if (!result || result->generation != generation) return;
        if (!result->track) { fail(result->error); return; }
        readyTrack = std::move(result->track); busy = false; error.clear();
        status = OnlineSubtitleService::tr("字幕已下载并验证，正在加载");
        notify(); emit owner->subtitleReady();
    }
};

OnlineSubtitleService::OnlineSubtitleService(QString directory, QObject* parent)
    : OnlineSubtitleService(std::move(directory), TestOptions{}, parent) {}
OnlineSubtitleService::OnlineSubtitleService(QString directory, const TestOptions& options, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this, std::move(directory), options)) {}
OnlineSubtitleService::~OnlineSubtitleService() = default;
bool OnlineSubtitleService::available() const { return !impl_->media.isEmpty(); }
bool OnlineSubtitleService::configured() const { return validToken(impl_->token); }
bool OnlineSubtitleService::busy() const { return impl_->busy; }
QString OnlineSubtitleService::query() const { return impl_->query; }
void OnlineSubtitleService::setQuery(const QString& value) { const auto query = value.left(200); if (query != impl_->query) { impl_->query = query; emit changed(); } }
QString OnlineSubtitleService::status() const { return impl_->status; }
QString OnlineSubtitleService::error() const { return impl_->error; }
QString OnlineSubtitleService::selectedTitle() const { return impl_->selectedTitle; }
QVariantList OnlineSubtitleService::results() const { return impl_->results; }
QVariantList OnlineSubtitleService::files() const { return impl_->files; }
QString OnlineSubtitleService::currentMedia() const { return impl_->media; }
void OnlineSubtitleService::setMedia(const QString& localMedia) {
    const QFileInfo input(localMedia);
    const QString media = !localMedia.isEmpty() && input.isFile() ? input.absoluteFilePath() : QString{};
    if (media == impl_->media) return;
    impl_->invalidate(); impl_->media = media; impl_->query = media.isEmpty() ? QString{} : cleanQuery(media);
    impl_->results.clear(); impl_->files.clear(); impl_->downloads.clear(); impl_->selectedTitle.clear();
    impl_->status = media.isEmpty() ? tr("请先打开本地视频") : tr("确认片名后点击搜索；仅发送搜索词，不上传视频");
    emit changed();
}
std::optional<engine::SubtitleTrack> OnlineSubtitleService::takeReadyTrack() {
    auto result = std::move(impl_->readyTrack); impl_->readyTrack.reset();
    if (result) { impl_->status = tr("已加载在线字幕，可在字幕设置中调整时间"); emit changed(); }
    return result;
}
bool OnlineSubtitleService::configureToken(const QString& supplied) {
    auto plain = supplied.trimmed().toLatin1();
    if (!validToken(plain)) { impl_->fail(tr("请输入本人 ASSRT 的 32 位字母数字 Token")); return false; }
    const auto encrypted = protectToken(plain, true);
    QSaveFile saved(impl_->tokenPath());
    if (encrypted.isEmpty() || !QDir().mkpath(impl_->dataDirectory) || !saved.open(QIODevice::WriteOnly) ||
        saved.write(encrypted) != encrypted.size() || !saved.commit()) {
        SecureZeroMemory(plain.data(), size_t(plain.size())); impl_->fail(tr("无法加密保存 Token，请检查数据目录权限")); return false;
    }
    impl_->invalidate();
    if (!impl_->token.isEmpty()) SecureZeroMemory(impl_->token.data(), size_t(impl_->token.size()));
    impl_->token = plain;
    impl_->status = tr("本人 Token 已加密保存，可以搜索字幕"); emit changed(); return true;
}
void OnlineSubtitleService::clearToken() {
    impl_->invalidate();
    if (!impl_->token.isEmpty()) SecureZeroMemory(impl_->token.data(), size_t(impl_->token.size()));
    impl_->token.clear();
    if (QFile::exists(impl_->tokenPath()) && !QFile::remove(impl_->tokenPath())) impl_->error = tr("当前 Token 已清除，但磁盘文件删除失败，请检查数据目录");
    impl_->status = tr("已清除 Token"); emit changed();
}
void OnlineSubtitleService::cancel() { const bool wasBusy = impl_->busy; impl_->invalidate(); if (wasBusy) impl_->status = tr("已取消"); emit changed(); }
void OnlineSubtitleService::search() {
    impl_->invalidate(); impl_->results.clear(); impl_->files.clear(); impl_->downloads.clear(); impl_->selectedTitle.clear();
    if (!impl_->admitted()) return;
    const QString query = impl_->query.simplified();
    if (query.size() < 3) { impl_->fail(tr("请输入至少 3 个字符的片名")); return; }
    QUrlQuery args; args.addQueryItem(QStringLiteral("q"), query); args.addQueryItem(QStringLiteral("cnt"), QStringLiteral("15")); args.addQueryItem(QStringLiteral("no_muxer"), QStringLiteral("1"));
    impl_->busy = true; impl_->status = tr("正在搜索字幕…"); emit changed();
    impl_->request(impl_->endpoint(QStringLiteral("v1/sub/search"), args), true, kMaxMetadata, impl_->generation,
        [this](QByteArray bytes) {
            const auto subs = impl_->subtitleResponse(bytes); if (!subs) return;
            for (const auto& value : *subs) {
                const auto object = value.toObject();
                const QString id = object.value(QStringLiteral("id")).toVariant().toString();
                if (!QRegularExpression(QStringLiteral("^[0-9]{1,12}$")).match(id).hasMatch()) continue;
                QString title = object.value(QStringLiteral("native_name")).toString();
                if (title.isEmpty()) title = object.value(QStringLiteral("videoname")).toString();
                if (title.isEmpty()) title = tr("字幕 %1").arg(id);
                impl_->results << QVariantMap{{"title", title.left(500)}, {"language", object.value(QStringLiteral("lang")).toObject().value(QStringLiteral("desc")).toString().left(100)},
                    {"format", object.value(QStringLiteral("subtype")).toString().left(100)}, {"id", id}};
                if (impl_->results.size() >= 15) break;
            }
            impl_->busy = false; impl_->status = impl_->results.isEmpty() ? tr("没有找到字幕，请修改片名再搜索") : tr("找到 %1 个结果，请选择匹配版本").arg(impl_->results.size()); emit changed();
        });
}
void OnlineSubtitleService::chooseResult(int index) {
    impl_->invalidate(); impl_->files.clear(); impl_->downloads.clear();
    if (!impl_->admitted()) return;
    if (index < 0 || index >= impl_->results.size()) { impl_->fail(tr("请选择有效的搜索结果")); return; }
    const auto result = impl_->results[index].toMap(); impl_->selectedTitle = result.value(QStringLiteral("title")).toString();
    QUrlQuery args; args.addQueryItem(QStringLiteral("id"), result.value(QStringLiteral("id")).toString());
    impl_->busy = true; impl_->status = tr("正在读取字幕文件列表…"); emit changed();
    impl_->request(impl_->endpoint(QStringLiteral("v1/sub/detail"), args), true, kMaxMetadata, impl_->generation,
        [this](QByteArray bytes) {
            const auto subs = impl_->subtitleResponse(bytes); if (!subs) return;
            if (subs->isEmpty()) { impl_->fail(tr("字幕详情为空，请重新选择")); return; }
            const auto detail = subs->first().toObject();
            QJsonArray entries = detail.value(QStringLiteral("filelist")).toArray();
            if (entries.isEmpty() && !detail.value(QStringLiteral("url")).toString().isEmpty())
                entries.append(QJsonObject{{"f", detail.value(QStringLiteral("filename"))}, {"s", detail.value(QStringLiteral("size"))}, {"url", detail.value(QStringLiteral("url"))}});
            int supported = 0;
            for (const auto& value : entries) {
                const auto entry = value.toObject(); const QString name = entry.value(QStringLiteral("f")).toString();
                const QString extension = QFileInfo(name).suffix().toLower();
                QUrl url(entry.value(QStringLiteral("url")).toString());
                const bool text = QStringList{QStringLiteral("srt"), QStringLiteral("ass"), QStringLiteral("ssa"), QStringLiteral("vtt")}.contains(extension);
                const auto size = declaredFileSize(entry.value(QStringLiteral("s")));
                const bool sizeOk = !size || *size <= kMaxSubtitle;
                const bool allowed = text && sizeOk && impl_->safeDownload(url);
                supported += allowed;
                impl_->files << QVariantMap{{"name", name.left(500)}, {"size", size.value_or(0)}, {"supported", allowed}};
                QString displayName = name;
                displayName.replace(QLatin1Char('\\'), QLatin1Char('/'));
                displayName = displayName.section(QLatin1Char('/'), -1);
                displayName.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1F\\x7F]")));
                displayName = displayName.trimmed().left(200);
                if (displayName.isEmpty()) displayName = tr("在线字幕");
                impl_->downloads.push_back({allowed ? url : QUrl{}, extension, displayName});
                if (impl_->files.size() >= 128) break;
            }
            impl_->busy = false; impl_->status = supported ? tr("请选择一个字幕文件下载并加载") : tr("此结果没有可直接下载的文本字幕；本版不解压 ZIP/RAR/7z，请换一个结果"); emit changed();
        });
}
void OnlineSubtitleService::downloadFile(int index) {
    impl_->invalidate();
    if (!impl_->admitted()) return;
    if (index < 0 || size_t(index) >= impl_->downloads.size() || !impl_->files[index].toMap().value(QStringLiteral("supported")).toBool()) {
        impl_->fail(tr("该文件不是可安全下载的 SRT/ASS/SSA/VTT 字幕，请选择其他文件")); return;
    }
    auto download = impl_->downloads[size_t(index)];
    if (!impl_->safeDownload(download.url)) { impl_->fail(tr("字幕下载地址不受支持，请重新选择结果")); return; }
    impl_->busy = true; impl_->status = tr("正在下载字幕…"); emit changed();
    const quint64 id = impl_->generation;
    impl_->request(download.url, false, kMaxSubtitle, id,
        [this, id, extension = download.extension, displayName = download.displayName](QByteArray bytes) {
            impl_->status = tr("正在验证字幕编码与内容…"); emit changed();
            { std::lock_guard lock(impl_->parseMutex); impl_->pending = Impl::ParseJob{id, std::move(bytes), extension, displayName}; }
            impl_->parseWake.notify_one();
        });
}
} // namespace veyra::ui
