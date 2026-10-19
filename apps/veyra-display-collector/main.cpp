#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <shellapi.h>
#include <evntrace.h>

#include <QByteArrayView>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr qint64 kMaxLine = 32 * 1024;
constexpr qint64 kMaxPending = 512 * 1024;
constexpr char kPresentMonSha256[] =
    "9bec3083069f58f911e6a512f4806db51a27bd096103087bc1d05ef54c80a191";

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { reset(); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE replacement = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = replacement;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

struct Arguments {
    QString pipe;
    QString nonce;
    DWORD parentPid = 0;
    quint64 parentCreated = 0;
    int maxSeconds = 86400;
};

bool parseArguments(const QStringList& values, Arguments& result) {
    if (values.size() != 9 && values.size() != 11) return false;
    QSet<QString> seen;
    for (qsizetype i = 1; i < values.size(); i += 2) {
        const QString name = values[i];
        if (seen.contains(name)) return false;
        seen.insert(name);
        const QString value = values[i + 1];
        if (name == QLatin1String("--pipe")) result.pipe = value;
        else if (name == QLatin1String("--nonce")) result.nonce = value;
        else if (name == QLatin1String("--parent-pid")) {
            bool ok = false;
            const quint64 parsed = value.toULongLong(&ok, 10);
            if (!ok || !parsed || parsed > MAXDWORD ||
                !QRegularExpression(QStringLiteral("^[0-9]+$")).match(value).hasMatch()) return false;
            result.parentPid = static_cast<DWORD>(parsed);
        } else if (name == QLatin1String("--parent-created")) {
            bool ok = false;
            result.parentCreated = value.toULongLong(&ok, 10);
            if (!ok || !result.parentCreated ||
                !QRegularExpression(QStringLiteral("^[0-9]+$")).match(value).hasMatch()) return false;
        } else if (name == QLatin1String("--max-seconds")) {
            bool ok = false;
            result.maxSeconds = value.toInt(&ok, 10);
            if (!ok || result.maxSeconds < 15 || result.maxSeconds > 86400) return false;
        } else return false;
    }
    if (!seen.contains(QStringLiteral("--pipe")) ||
        !seen.contains(QStringLiteral("--nonce")) ||
        !seen.contains(QStringLiteral("--parent-pid")) ||
        !seen.contains(QStringLiteral("--parent-created"))) return false;
    return result.parentPid != GetCurrentProcessId() &&
        QRegularExpression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")).match(result.pipe).hasMatch() &&
        QRegularExpression(QStringLiteral("^[A-Fa-f0-9]{64}$")).match(result.nonce).hasMatch();
}

QString canonicalPath(const QString& path) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QString{} : QDir::cleanPath(QDir::fromNativeSeparators(canonical));
}

bool samePath(const QString& a, const QString& b) {
    return !a.isEmpty() && !b.isEmpty() && QString::compare(a, b, Qt::CaseInsensitive) == 0;
}

QString processPath(HANDLE process) {
    wchar_t path[32768]{};
    DWORD count = static_cast<DWORD>(std::size(path));
    if (!QueryFullProcessImageNameW(process, 0, path, &count)) return {};
    return canonicalPath(QString::fromWCharArray(path, static_cast<qsizetype>(count)));
}

quint64 creationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

bool isElevated() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return false;
    TOKEN_ELEVATION elevated{};
    DWORD size = 0;
    return GetTokenInformation(token.value, TokenElevation, &elevated, sizeof(elevated), &size) &&
        elevated.TokenIsElevated != 0;
}

// A synchronous ControlTrace can wait for a provider. Keep it off the Qt loop;
// the helper's hard exit deadline remains effective. This thread never touches
// Qt objects, and it receives only our validated, nonce-derived session name.
DWORD WINAPI stopOwnTrace(void* parameter) {
    std::unique_ptr<std::wstring> session(static_cast<std::wstring*>(parameter));
    struct Properties {
        EVENT_TRACE_PROPERTIES data{};
        wchar_t name[128]{};
    } properties;
    properties.data.Wnode.BufferSize = sizeof(properties);
    properties.data.LoggerNameOffset = static_cast<ULONG>(offsetof(Properties, name));
    ControlTraceW(0, session->c_str(), &properties.data, EVENT_TRACE_CONTROL_STOP);
    return 0;
}

class Collector final : public QObject {
public:
    Collector(QCoreApplication& app, Arguments arguments)
        : QObject(&app), arguments_(std::move(arguments)) {
        connectionTimer_.setSingleShot(true);
        connectionTimer_.setInterval(5000);
        commandTimer_.setSingleShot(true);
        commandTimer_.setInterval(10000);
        runTimer_.setSingleShot(true);
        cleanupTimer_.setSingleShot(true);
        cleanupTimer_.setInterval(3000);
        finalTimer_.setSingleShot(true);
        finalTimer_.setInterval(100);
        parentTimer_.setInterval(250);
        socket_.setReadBufferSize(kMaxLine + 1);
        configureProcess(capture_);
        configureProcess(cleanup_);
        connect(&connectionTimer_, &QTimer::timeout, this, [this] { stop(1, QStringLiteral("Parent pipe connection timed out.")); });
        connect(&commandTimer_, &QTimer::timeout, this, [this] { stop(1, QStringLiteral("Authenticated start command timed out.")); });
        connect(&runTimer_, &QTimer::timeout, this, [this] { stop(0, QStringLiteral("Collector maximum duration reached.")); });
        connect(&parentTimer_, &QTimer::timeout, this, [this] {
            if (!parent_ || WaitForSingleObject(parent_.value, 0) != WAIT_TIMEOUT)
                stop(0, QStringLiteral("Parent exited."));
            if (!finishing_ && socket_.bytesToWrite() > kMaxPending)
                stop(1, QStringLiteral("Parent pipe backlog exceeded its bound."));
        });
        connect(&socket_, &QLocalSocket::connected, this, [this] { connected(); });
        connect(&socket_, &QLocalSocket::readyRead, this, [this] { commands(); });
        connect(&socket_, &QLocalSocket::disconnected, this, [this] {
            stop(authenticated_ ? 0 : 1, QStringLiteral("Parent pipe disconnected."));
        });
        connect(&socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
            stop(authenticated_ ? 0 : 1, QStringLiteral("Parent pipe became unavailable."));
        });
        connect(&capture_, &QProcess::started, this, [this] {
            captureStarted_ = true;
            if (!bindToJob(capture_)) stop(1, QStringLiteral("Cannot bound the capture child lifetime."));
        });
        connect(&capture_, &QProcess::readyReadStandardOutput, this, [this] { stdoutReady(); });
        connect(&capture_, &QProcess::readyReadStandardError, this, [this] { stderrReady(); });
        connect(&capture_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (!finishing_ && error == QProcess::FailedToStart)
                stop(1, QStringLiteral("PresentMon could not start."));
        });
        connect(&capture_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            if (finishing_) return;
            stdoutReady();
            stderrReady();
            if (finishing_) return;
            if (!stderrBuffer_.isEmpty()) diagnostic(QString::fromUtf8(stderrBuffer_));
            const bool denied = permissionSeen_ || code == 6;
            stop(denied ? 6 : (code == 0 && status == QProcess::NormalExit ? 0 : 1),
                denied ? QStringLiteral("Display capture requires permission.") :
                code == 0 ? QStringLiteral("Display capture completed.") : QStringLiteral("PresentMon exited with an error."));
        });
        connect(&cleanup_, &QProcess::started, this, [this] {
            if (!bindToJob(cleanup_)) {
                cleanup_.kill();
                fallbackStop();
                finalTimer_.start();
            }
        });
        connect(&cleanup_, &QProcess::readyReadStandardOutput, this, [this] { cleanup_.readAllStandardOutput(); });
        connect(&cleanup_, &QProcess::readyReadStandardError, this, [this] { cleanup_.readAllStandardError(); });
        connect(&cleanup_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                fallbackStop();
                finalTimer_.start();
            }
        });
        connect(&cleanup_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            if (code != 0 || status != QProcess::NormalExit) fallbackStop();
            finalTimer_.start();
        });
        connect(&cleanupTimer_, &QTimer::timeout, this, [this] {
            fallbackStop();
            if (cleanup_.state() != QProcess::NotRunning) cleanup_.kill();
            finish();
        });
        connect(&finalTimer_, &QTimer::timeout, this, [this] { finish(); });
    }

    bool initialize() {
        wchar_t executable[32768]{};
        const DWORD count = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        if (!count || count >= std::size(executable)) return false;
        const QString ownPath = canonicalPath(QString::fromWCharArray(executable, static_cast<qsizetype>(count)));
        if (ownPath.isEmpty()) return false;
        directory_ = QFileInfo(ownPath).absolutePath();
        const QString requestedParent = QDir(directory_).filePath(QStringLiteral("veyra_qml_ui.exe"));
        const QString expectedParent = canonicalPath(requestedParent);
        if (!samePath(expectedParent, QDir::cleanPath(requestedParent))) return false;
        parent_.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, arguments_.parentPid));
        if (!parent_ || WaitForSingleObject(parent_.value, 0) != WAIT_TIMEOUT ||
            creationTime(parent_.value) != arguments_.parentCreated ||
            !samePath(processPath(parent_.value), expectedParent)) return false;
        tool_ = QDir(directory_).filePath(QStringLiteral("tools/PresentMon.exe"));
        // Reject a redirecting tool path and keep the verified file open without
        // write/delete sharing until all child processes have exited.
        if (!samePath(canonicalPath(tool_), QDir::cleanPath(tool_))) return false;
        const std::wstring toolPath = QDir::toNativeSeparators(tool_).toStdWString();
        toolFile_.reset(CreateFileW(toolPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!toolFile_) return false;
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(toolFile_.value, &fileSize) || fileSize.QuadPart != 956768) return false;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        char bytes[64 * 1024];
        DWORD read = 0;
        for (;;) {
            if (!ReadFile(toolFile_.value, bytes, sizeof(bytes), &read, nullptr)) return false;
            if (!read) break;
            hash.addData(QByteArrayView(bytes, static_cast<qsizetype>(read)));
        }
        if (hash.result().toHex() != kPresentMonSha256) return false;
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job_) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 2;
        if (!SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
        session_ = QStringLiteral("VeyraDisplay-%1-%2").arg(arguments_.parentPid).arg(arguments_.nonce.left(32).toLower());
        return true;
    }

    void begin() {
        parentTimer_.start();
        connectionTimer_.start();
        socket_.connectToServer(arguments_.pipe, QIODevice::ReadWrite);
    }

private:
    static void configureProcess(QProcess& process) {
        process.setProcessChannelMode(QProcess::SeparateChannels);
        process.setStandardInputFile(QProcess::nullDevice());
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
            args->flags &= ~(CREATE_NEW_CONSOLE | DETACHED_PROCESS);
            args->flags |= CREATE_NO_WINDOW;
            args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            args->startupInfo->wShowWindow = SW_HIDE;
        });
    }

    bool bindToJob(QProcess& process) {
        const qint64 id = process.processId();
        if (id <= 0 || id > MAXDWORD) return false;
        Handle child;
        child.reset(OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(id)));
        return child && AssignProcessToJobObject(job_.value, child.value);
    }

    bool send(QJsonObject object) {
        if (!authenticated_ || socket_.state() != QLocalSocket::ConnectedState) return false;
        object.insert(QStringLiteral("schema"), 1);
        object.insert(QStringLiteral("nonce"), arguments_.nonce);
        const QByteArray wire = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        if (socket_.bytesToWrite() + wire.size() > kMaxPending) {
            if (!finishing_ && !backlogFailed_) {
                backlogFailed_ = true;
                QTimer::singleShot(0, this, [this] { stop(1, QStringLiteral("Parent pipe backlog exceeded its bound.")); });
            }
            return false;
        }
        return socket_.write(wire) == wire.size();
    }

    void state(const QString& value) {
        send({{QStringLiteral("type"), QStringLiteral("state")}, {QStringLiteral("state"), value}});
    }

    void diagnostic(QString message) {
        message = message.left(1024);
        send({{QStringLiteral("type"), QStringLiteral("diagnostic")}, {QStringLiteral("message"), message}});
    }

    void connected() {
        connectionTimer_.stop();
        ULONG serverPid = 0;
        const qintptr descriptor = socket_.socketDescriptor();
        if (descriptor == -1 || !GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(descriptor), &serverPid) ||
            serverPid != arguments_.parentPid || !parent_ || WaitForSingleObject(parent_.value, 0) != WAIT_TIMEOUT ||
            creationTime(parent_.value) != arguments_.parentCreated) {
            stop(1, QStringLiteral("Parent pipe identity verification failed."));
            return;
        }
        authenticated_ = true;
        if (!send({{QStringLiteral("type"), QStringLiteral("ready")},
                   {QStringLiteral("elevated"), isElevated()},
                   {QStringLiteral("helperPid"), static_cast<qint64>(GetCurrentProcessId())}})) {
            stop(1, QStringLiteral("Cannot send collector readiness."));
            return;
        }
        commandTimer_.start();
    }

    void commands() {
        if (finishing_ || !authenticated_) return;
        commandBuffer_.append(socket_.readAll());
        while (!finishing_) {
            const qsizetype end = commandBuffer_.indexOf('\n');
            if (end < 0) {
                if (commandBuffer_.size() > kMaxLine) stop(1, QStringLiteral("Oversized parent command."));
                return;
            }
            if (end > kMaxLine) { stop(1, QStringLiteral("Oversized parent command.")); return; }
            QByteArray line = commandBuffer_.left(end);
            commandBuffer_.remove(0, end + 1);
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(line, &error);
            const QJsonObject command = document.object();
            if (error.error != QJsonParseError::NoError || !document.isObject() || command.size() != 3 ||
                !command.value(QStringLiteral("schema")).isDouble() || command.value(QStringLiteral("schema")).toDouble() != 1.0 ||
                !command.value(QStringLiteral("nonce")).isString() || command.value(QStringLiteral("nonce")).toString() != arguments_.nonce ||
                !command.value(QStringLiteral("type")).isString()) {
                stop(1, QStringLiteral("Invalid authenticated parent command.")); return;
            }
            const QString type = command.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("stop")) { stop(0, QStringLiteral("Parent requested stop.")); return; }
            if (type != QLatin1String("start") || requested_) {
                stop(1, QStringLiteral("Unsupported or repeated parent command.")); return;
            }
            requested_ = true;
            commandTimer_.stop();
            state(QStringLiteral("starting"));
            capture_.setWorkingDirectory(directory_);
            capture_.setProgram(tool_);
            capture_.setArguments({QStringLiteral("--process_id"), QString::number(arguments_.parentPid),
                QStringLiteral("--session_name"), session_, QStringLiteral("--output_stdout"),
                QStringLiteral("--v2_metrics"), QStringLiteral("--qpc_time_ms"), QStringLiteral("--track_frame_type"),
                QStringLiteral("--no_console_stats"), QStringLiteral("--no_track_gpu"), QStringLiteral("--no_track_input"),
                QStringLiteral("--terminate_on_proc_exit"), QStringLiteral("--timed"), QString::number(arguments_.maxSeconds),
                QStringLiteral("--terminate_after_timed")});
            runTimer_.start(arguments_.maxSeconds * 1000);
            capture_.start();
        }
    }

    void stdoutReady() {
        if (finishing_) { capture_.readAllStandardOutput(); return; }
        stdoutBuffer_.append(capture_.readAllStandardOutput());
        while (!finishing_) {
            const qsizetype end = stdoutBuffer_.indexOf('\n');
            if (end < 0) {
                if (stdoutBuffer_.size() > kMaxLine) stop(1, QStringLiteral("Oversized PresentMon CSV line."));
                return;
            }
            if (end > kMaxLine) { stop(1, QStringLiteral("Oversized PresentMon CSV line.")); return; }
            QByteArray line = stdoutBuffer_.left(end);
            stdoutBuffer_.remove(0, end + 1);
            if (line.endsWith('\r')) line.chop(1);
            const QString text = QString::fromUtf8(line);
            if (!send({{QStringLiteral("type"), QStringLiteral("csv")}, {QStringLiteral("line"), text}})) {
                stop(1, QStringLiteral("Cannot forward bounded CSV data.")); return;
            }
            if (!headerSeen_ && text.startsWith(QLatin1String("Application,")) &&
                text.contains(QLatin1String("SwapChainAddress")) && text.contains(QLatin1String("DisplayedTime"))) {
                headerSeen_ = true;
                state(QStringLiteral("live"));
            }
        }
    }

    void stderrReady() {
        if (finishing_) { capture_.readAllStandardError(); return; }
        stderrBuffer_.append(capture_.readAllStandardError());
        while (!finishing_) {
            const qsizetype end = stderrBuffer_.indexOf('\n');
            if (end < 0) {
                if (stderrBuffer_.size() > kMaxLine) stop(1, QStringLiteral("Oversized PresentMon diagnostic line."));
                return;
            }
            if (end > kMaxLine) { stop(1, QStringLiteral("Oversized PresentMon diagnostic line.")); return; }
            const QString text = QString::fromUtf8(stderrBuffer_.left(end)).trimmed();
            stderrBuffer_.remove(0, end + 1);
            if (!text.isEmpty()) diagnostic(text);
            if (text.contains(QLatin1String("access denied"), Qt::CaseInsensitive) ||
                text.contains(QLatin1String("access is denied"), Qt::CaseInsensitive)) {
                permissionSeen_ = true;
                stop(6, QStringLiteral("Display capture requires permission."));
            }
        }
    }

    void fallbackStop() {
        if (!captureStarted_ || fallbackRequested_) return;
        fallbackRequested_ = true;
        auto name = std::make_unique<std::wstring>(session_.toStdWString());
        HANDLE thread = CreateThread(nullptr, 0, stopOwnTrace, name.get(), 0, nullptr);
        if (thread) {
            name.release();
            CloseHandle(thread);
        }
    }

    void stop(int code, const QString& reason) {
        if (finishing_) return;
        finishing_ = true;
        exitCode_ = code;
        connectionTimer_.stop();
        commandTimer_.stop();
        runTimer_.stop();
        parentTimer_.stop();
        if (code == 6) state(QStringLiteral("permission"));
        else if (code != 0) state(QStringLiteral("error"));
        if (!reason.isEmpty()) diagnostic(reason);
        if (!captureStarted_) {
            if (capture_.state() != QProcess::NotRunning) capture_.kill();
            finalTimer_.start();
            return;
        }
        // This command can affect only the private nonce-derived session.
        cleanup_.setWorkingDirectory(directory_);
        cleanup_.setProgram(tool_);
        cleanup_.setArguments({QStringLiteral("--session_name"), session_, QStringLiteral("--terminate_existing_session")});
        cleanupTimer_.start();
        cleanup_.start();
    }

    void finish() {
        if (exiting_) return;
        exiting_ = true;
        cleanupTimer_.stop();
        finalTimer_.stop();
        if (captureStarted_ && capture_.state() != QProcess::NotRunning) fallbackStop();
        if (capture_.state() != QProcess::NotRunning) capture_.kill();
        if (cleanup_.state() != QProcess::NotRunning) cleanup_.kill();
        job_.reset(); // kill-on-close covers only children assigned by this helper.
        if (capture_.state() != QProcess::NotRunning) capture_.waitForFinished(150);
        if (cleanup_.state() != QProcess::NotRunning) cleanup_.waitForFinished(150);
        send({{QStringLiteral("type"), QStringLiteral("state")}, {QStringLiteral("state"), QStringLiteral("stopped")},
              {QStringLiteral("exitCode"), exitCode_}});
        socket_.flush();
        if (socket_.bytesToWrite() > 0) socket_.waitForBytesWritten(100);
        socket_.abort();
        toolFile_.reset();
        parent_.reset();
        // Do not enter QProcess destructor's long platform teardown on a
        // hostile/stuck child. All our children have been killed and the job
        // closed; exiting this dedicated helper cannot terminate the player.
        ExitProcess(static_cast<UINT>(exitCode_));
    }

    Arguments arguments_;
    Handle parent_, toolFile_, job_;
    QString directory_, tool_, session_;
    QLocalSocket socket_;
    QProcess capture_, cleanup_;
    QTimer connectionTimer_, commandTimer_, runTimer_, parentTimer_, cleanupTimer_, finalTimer_;
    QByteArray commandBuffer_, stdoutBuffer_, stderrBuffer_;
    bool authenticated_ = false;
    bool requested_ = false;
    bool captureStarted_ = false;
    bool headerSeen_ = false;
    bool permissionSeen_ = false;
    bool backlogFailed_ = false;
    bool fallbackRequested_ = false;
    bool finishing_ = false;
    bool exiting_ = false;
    int exitCode_ = 1;
};
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide || argc < 1) { if (wide) LocalFree(wide); return 1; }
    std::vector<QByteArray> storage;
    storage.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) storage.push_back(QString::fromWCharArray(wide[i]).toUtf8());
    LocalFree(wide);
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);
    QCoreApplication app(argc, argv.data());
    Arguments arguments;
    if (!parseArguments(app.arguments(), arguments)) return 1;
    Collector collector(app, std::move(arguments));
    if (!collector.initialize()) return 1;
    collector.begin();
    return app.exec();
}
