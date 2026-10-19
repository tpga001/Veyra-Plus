#include "veyra/ui/VideoOutputPublisher.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

using veyra::ui::VideoOutputPublisher;
using veyra::ui::VideoOutputSource;
namespace {
int checks = 0, failures = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL " << message << '\n'; }
}
QString child(const QString& root, const char* name) { return QDir(root).filePath(QString::fromUtf8(name)); }
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(4097) : QByteArray{};
}
bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
QJsonObject object(const QString& path) { return QJsonDocument::fromJson(read(path)).object(); }
bool waitFor(const std::function<bool()>& predicate, int milliseconds = 3000) {
    QElapsedTimer clock; clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        if (predicate()) return true;
        QThread::msleep(10);
    }
    return predicate();
}
QString heartbeat(const QString& root) {
    const QDir directory(child(root, "display-targets"));
    const auto files = directory.entryList({QStringLiteral("*.json")}, QDir::Files);
    return files.size() == 1 ? directory.filePath(files.front()) : QString();
}
// Same ordinary-user NTFS junction setup as LocalPlayerProfileTests: no shell,
// Developer Mode, GPU, collector or application process is needed.
bool junction(const QString& link, const QString& target) {
#ifdef Q_OS_WIN
    if (!QDir().mkpath(link)) return false;
    const auto native = QDir::toNativeSeparators(link).toStdWString();
    const auto print = QDir::toNativeSeparators(QFileInfo(target).absoluteFilePath()).toStdWString();
    const auto substitute = std::wstring(L"\\??\\") + print;
    const auto text = substitute + L'\0' + print + L'\0';
    struct Data { DWORD tag; WORD bytes, reserved, subOffset, subLength, printOffset, printLength; WCHAR text[1]; };
    QByteArray buffer(int(offsetof(Data, text) + text.size() * sizeof(wchar_t)), '\0');
    auto* data = reinterpret_cast<Data*>(buffer.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT; data->bytes = WORD(buffer.size() - 8);
    data->subLength = WORD(substitute.size() * sizeof(wchar_t));
    data->printOffset = WORD((substitute.size() + 1) * sizeof(wchar_t));
    data->printLength = WORD(print.size() * sizeof(wchar_t));
    std::memcpy(data->text, text.data(), text.size() * sizeof(wchar_t));
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data, DWORD(buffer.size()),
                                    nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle); return ok;
#else
    return QFile::link(target, link);
#endif
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        std::cerr << "usage: veyra_video_output_publisher_tests <new absolute E-drive scratch directory>\n";
        return 2;
    }
    const auto scratch = QDir::cleanPath(app.arguments().at(1));
    if (!QDir::isAbsolutePath(scratch) || !scratch.startsWith(QStringLiteral("E:/"), Qt::CaseInsensitive)
        || QFileInfo::exists(scratch) || !QDir().mkpath(scratch)) {
        std::cerr << "requires a new absolute E-drive scratch directory\n"; return 2;
    }
    quint32 actualPid = 0; quint64 actualCreated = 0;
#ifdef Q_OS_WIN
    FILETIME c{}, e{}, k{}, u{};
    check(GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) != FALSE, "read actual process identity");
    actualPid = GetCurrentProcessId(); actualCreated = (quint64(c.dwHighDateTime) << 32) | c.dwLowDateTime;
#endif
    const auto resident = child(scratch, "resident");
    VideoOutputSource source;
    source.target.pid = 999999; // Must never override the real publisher process.
    source.target.application = "F:/private-video-title.mp4";
    source.target.sessionId = 18446744073709551615ULL;
    source.target.epoch = 9007199254740993ULL;
    source.target.revision = 7; source.target.generation = 3;
    source.target.outputSwapChain = 0xfedcba9876543210ULL;
    source.available = source.playing = source.frameGenerationActive = true;
    source.sourceKind = "browser"; source.frameGenerationBackend = "xess";
    source.expectedGeneratedFrameType = "Intel XeSS-FG";
    source.videoHwnd = 0x123456789abULL; source.sourceHwnd = 0xabc;
    const auto before = QDateTime::currentMSecsSinceEpoch();
    auto publisher = std::make_unique<VideoOutputPublisher>(resident);
    publisher->setSourceProvider([&] { return source; });
    check(waitFor([&] { return !object(heartbeat(resident)).isEmpty(); }), "worker publishes real atomic JSON");
    const auto path = heartbeat(resident);
    auto packet = object(path);
    check(packet.value("schema").toInt() == 1 && packet.value("pid").toInteger() == actualPid,
          "schema and actual PID, not supplied PID");
    check(packet.value("processCreatedFileTime").toString() == QString::number(actualCreated), "actual process creation FILETIME string");
    check(QFileInfo(path).fileName() == QStringLiteral("%1-%2.json").arg(actualPid).arg(actualCreated), "identity filename isolates processes and PID reuse");
    check(packet.value("sessionId").toString() == QStringLiteral("18446744073709551615")
          && packet.value("epoch").toString() == QStringLiteral("9007199254740993"), "uint64 values preserve full precision as strings");
    check(packet.value("outputSwapchain").toString() == QStringLiteral("0xfedcba9876543210")
          && packet.value("outputGeneration").toString() == QStringLiteral("3"), "swapchain hexadecimal and generation string");
    check(packet.value("videoHwnd").toString() == QStringLiteral("0x123456789ab")
          && packet.value("sourceHwnd").toString() == QStringLiteral("0xabc"), "window handles preserve addresses");
    check(packet.value("role").toString() == QStringLiteral("browser-resident")
          && packet.value("sourceKind").toString() == QStringLiteral("browser"), "resident role and browser source");
    check(packet.value("available").toBool() && packet.value("playing").toBool()
          && packet.value("frameGenerationActive").toBool()
          && packet.value("frameGenerationBackend").toString() == QStringLiteral("xess")
          && packet.value("expectedGeneratedFrameType").toString() == QStringLiteral("Intel XeSS-FG"), "provider generation facts preserved");
    check(packet.value("capturedUtcMs").toInteger() >= before
          && packet.value("capturedUtcMs").toInteger() <= QDateTime::currentMSecsSinceEpoch(), "capture time recorded at snapshot");
    check(packet.value("sequence").isString() && packet.value("sequence").toString().toULongLong() >= 1, "publication sequence is a positive decimal string");
    check(read(path).size() <= 4096 && !read(path).contains("private-video-title")
          && !packet.contains("application") && !packet.contains("filePath"), "tiny output descriptor contains no media history or path");
    const auto previousTime = packet.value("capturedUtcMs").toInteger();
    check(waitFor([&] { return object(path).value("sequence").toString().toULongLong() >= 2; }), "500ms heartbeat advances without UI or GPU activity");
    check(object(path).value("capturedUtcMs").toInteger() > previousTime, "each sampled heartbeat has its own capture time");
    source.target.sessionId = 8; source.target.epoch = 4; source.target.revision = 21;
    source.target.generation = 5; source.target.outputSwapChain = 0x456;
    source.playing = false;
    publisher->setSourceProvider([&] { return source; });
    check(waitFor([&] { return object(path).value("revision").toString() == QStringLiteral("21"); }), "changed identity reaches disk");
    packet = object(path);
    check(packet.value("sessionId").toString() == QStringLiteral("8") && packet.value("epoch").toString() == QStringLiteral("4")
          && packet.value("outputGeneration").toString() == QStringLiteral("5")
          && packet.value("outputSwapchain").toString() == QStringLiteral("0x456"), "new source identity never borrows old fields");
    check(packet.value("available").toBool() && !packet.value("playing").toBool(), "pause keeps live identity heartbeat for consumer freezing");
    for (int i = 0; i < 200; ++i) {
        source.target.revision = 1000 + i;
        publisher->setSourceProvider([&] { return source; });
    }
    check(waitFor([&] { return object(path).value("revision").toString() == QStringLiteral("1199"); }), "capacity-one mailbox converges to latest snapshot");
    source.available = false; source.playing = true;
    publisher->setSourceProvider([&] { return source; });
    check(waitFor([&] { const auto p = object(path); return !p.value("available").toBool() && !p.value("playing").toBool(); }), "unavailable output cannot claim playback");
    check(!object(path).value("frameGenerationActive").toBool()
          && object(path).value("expectedGeneratedFrameType").toString().isEmpty(), "inactive output cannot claim generated stream");
    source.sourceKind = "F:/private-title.mp4"; source.frameGenerationBackend = "F:/private-backend";
    publisher->setSourceProvider([&] { return source; });
    check(waitFor([&] { return object(path).value("sourceKind").toString() == QStringLiteral("none"); }), "unknown source kind is rejected");
    check(!object(path).value("available").toBool() && object(path).value("frameGenerationBackend").toString().isEmpty()
          && !read(path).contains("private-title"), "unsupported provider strings cannot disclose media paths");
    source.sourceKind = "local"; source.frameGenerationBackend = "xess";
    const auto other = child(QFileInfo(path).absolutePath(), "other-process.json");
    check(write(other, "other process"), "create unrelated heartbeat sentinel");
    publisher.reset();
    check(!QFileInfo::exists(path) && read(other) == "other process", "destructor removes only its owned heartbeat");

    source.available = source.playing = true; source.sourceKind = "local";
    const auto local = child(scratch, "local");
    publisher = std::make_unique<VideoOutputPublisher>(local, QStringLiteral("local-player"));
    publisher->setSourceProvider([&] { return source; });
    check(waitFor([&] { return !object(heartbeat(local)).isEmpty(); }), "local process role publishes without resident HTTP listener");
    const auto localPath = heartbeat(local);
    check(object(localPath).value("role").toString() == QStringLiteral("local-player"), "local role is explicit");
    // With no event processing between replacement and destruction, the UI timer
    // cannot submit a new snapshot that could overwrite this foreign sentinel.
    check(write(localPath, "foreign replacement"), "replace heartbeat after successful publication");
    publisher.reset();
    check(read(localPath) == "foreign replacement", "cleanup refuses a replaced heartbeat");

    const auto invalid = child(scratch, "invalid-role");
    { VideoOutputPublisher bad(invalid, QStringLiteral("unknown")); bad.setSourceProvider([&] { return source; }); }
    check(!QFileInfo::exists(invalid), "unknown role performs no filesystem work");
    { VideoOutputPublisher empty(QString{}); empty.setSourceProvider([&] { return source; }); }
    { VideoOutputPublisher relative(QStringLiteral("relative-publisher-root")); relative.setSourceProvider([&] { return source; }); }
    check(!QFileInfo::exists(QStringLiteral("relative-publisher-root")), "empty and relative roots rejected");
    const auto blocked = child(scratch, "root-is-file"); check(write(blocked, "sentinel"), "create non-directory root");
    { VideoOutputPublisher bad(blocked); bad.setSourceProvider([&] { return source; }); waitFor([] { return false; }, 120); }
    check(read(blocked) == "sentinel", "non-directory root preserved");
    const auto target = child(scratch, "junction-target"), link = child(scratch, "junction-link");
    check(QDir().mkpath(target) && junction(link, target), "create ordinary-user reparse ancestor fixture");
    { VideoOutputPublisher bad(child(link, "nested")); bad.setSourceProvider([&] { return source; }); waitFor([] { return false; }, 150); }
    check(!QFileInfo::exists(child(target, "nested")), "all reparse ancestors rejected before any writes");
#ifdef Q_OS_WIN
    check(RemoveDirectoryW(QDir::toNativeSeparators(link).toStdWString().c_str()) != FALSE, "remove junction itself without following target");
#endif
    std::cout << "checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
