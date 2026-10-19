#include "veyra/ui/VideoOutputPublisher.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace veyra::ui {
namespace {
constexpr qsizetype kMaximumPacketBytes = 4096;

bool processIdentity(quint32& pid, quint64& created) {
#ifdef Q_OS_WIN
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return false;
    pid = GetCurrentProcessId();
    created = (quint64(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    return pid && created;
#else
    Q_UNUSED(pid); Q_UNUSED(created);
    return false; // This Windows output contract must not invent a creation time.
#endif
}

bool safeAncestors(const QString& path) {
    QString current = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (;;) {
        const QFileInfo info(current);
        if (info.isSymbolicLink()) return false;
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(current).toStdWString();
        const DWORD attributes = GetFileAttributesW(native.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        const auto parent = QDir::cleanPath(info.absolutePath());
        if (parent == current) return true;
        current = parent;
    }
}

QString decimal(uint64_t value) { return QString::number(qulonglong(value)); }
QString address(uint64_t value) { return QStringLiteral("0x%1").arg(qulonglong(value), 0, 16); }
QString token(const std::string& value, bool spaces = false) {
    if (value.size() > 64) return {};
    for (const auto c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
              || c == '-' || c == '_' || (spaces && c == ' '))) return {};
    return QString::fromStdString(value);
}

bool writePacket(const QString& path, const QByteArray& bytes) {
    const QString directory = QFileInfo(path).absolutePath();
    if (!safeAncestors(path) || !QDir().mkpath(directory) || !safeAncestors(path)) return false;
    const QFileInfo existing(path);
    if (existing.exists() && !existing.isFile()) return false;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

void removeOwned(const QString& path, const QByteArray& written) {
    if (written.isEmpty() || !safeAncestors(path)) return;
#ifdef Q_OS_WIN
    // Open without FILE_SHARE_DELETE: inspect and delete this exact file handle,
    // not a path that might have been replaced after the ownership comparison.
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    HANDLE file = CreateFileW(native.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        && info.nFileSizeHigh == 0 && info.nFileSizeLow == DWORD(written.size())) {
        QByteArray actual(written.size(), '\0'); DWORD count = 0;
        if (ReadFile(file, actual.data(), DWORD(actual.size()), &count, nullptr)
            && count == DWORD(actual.size()) && actual == written) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            (void)SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
    CloseHandle(file);
#else
    Q_UNUSED(path); Q_UNUSED(written);
#endif
}
} // namespace

struct VideoOutputPublisher::Impl {
    QTimer timer;
    std::function<VideoOutputSource()> provider;
    QString path, role;
    quint32 pid = 0;
    quint64 created = 0, sequence = 0;
    std::mutex mutex;
    std::condition_variable pendingChanged;
    std::optional<QByteArray> pending;
    std::jthread writer;

    Impl(VideoOutputPublisher* owner, const QString& root, const QString& requestedRole) : role(requestedRole) {
        if (root.isEmpty() || root.contains(QChar(0)) || !QDir::isAbsolutePath(root)
            || (role != QStringLiteral("browser-resident") && role != QStringLiteral("local-player"))
            || !processIdentity(pid, created)) return;
        path = QDir(QDir::cleanPath(root)).filePath(QStringLiteral("display-targets/%1-%2.json").arg(pid).arg(created));
        writer = std::jthread([this](std::stop_token stop) {
            QByteArray written;
            for (;;) {
                QByteArray bytes;
                {
                    std::unique_lock lock(mutex);
                    pendingChanged.wait(lock, [&] { return stop.stop_requested() || pending.has_value(); });
                    if (stop.stop_requested()) break;
                    bytes = std::move(*pending); pending.reset();
                }
                if (writePacket(path, bytes)) written = std::move(bytes);
            }
            removeOwned(path, written);
        });
        timer.setInterval(500);
        QObject::connect(&timer, &QTimer::timeout, owner, [this] { sample(); });
        timer.start();
    }
    ~Impl() {
        timer.stop();
        if (writer.joinable()) {
            writer.request_stop(); pendingChanged.notify_one(); writer.join();
        }
    }
    void sample() {
        if (path.isEmpty() || !provider) return;
        const auto source = provider();
        const bool knownKind = source.sourceKind == "none" || source.sourceKind == "browser" || source.sourceKind == "local";
        const bool available = knownKind && source.available && source.target.sessionId && source.target.outputSwapChain;
        const bool generated = available && source.frameGenerationActive;
        const QJsonObject object{
            {"schema", 1}, {"pid", int(pid)}, {"processCreatedFileTime", decimal(created)}, {"role", role},
            {"sourceKind", knownKind ? QString::fromStdString(source.sourceKind) : QStringLiteral("none")},
            {"available", available}, {"playing", available && source.playing},
            {"sessionId", decimal(source.target.sessionId)}, {"epoch", decimal(source.target.epoch)},
            {"revision", decimal(source.target.revision)}, {"outputGeneration", decimal(source.target.generation)},
            {"outputSwapchain", address(source.target.outputSwapChain)},
            {"frameGenerationActive", generated}, {"frameGenerationBackend", token(source.frameGenerationBackend)},
            {"expectedGeneratedFrameType", generated ? token(source.expectedGeneratedFrameType, true) : QString()},
            {"videoHwnd", address(source.videoHwnd)}, {"sourceHwnd", address(source.sourceHwnd)},
            {"capturedUtcMs", QDateTime::currentMSecsSinceEpoch()}, {"sequence", decimal(++sequence)}};
        auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        if (bytes.size() > kMaximumPacketBytes) return;
        {
            std::lock_guard lock(mutex);
            pending = std::move(bytes); // Capacity one: old snapshots are replaced, never queued.
        }
        pendingChanged.notify_one();
    }
};

VideoOutputPublisher::VideoOutputPublisher(const QString& sharedDataDirectory, const QString& role, QObject* parent)
    : QObject(parent), p_(std::make_unique<Impl>(this, sharedDataDirectory, role)) {}
VideoOutputPublisher::~VideoOutputPublisher() = default;
void VideoOutputPublisher::setSourceProvider(std::function<VideoOutputSource()> provider) {
    p_->provider = std::move(provider); p_->sample();
}
} // namespace veyra::ui
