#include "veyra/ui/DesktopIntegration.h"

#include <QAction>
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMenu>
#include <QSaveFile>
#include <QProcess>
#include <QSet>
#include <QSessionManager>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <QWindowStateChangeEvent>

#ifdef Q_OS_WIN
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#endif

namespace veyra::ui {
namespace {
constexpr auto kRunEntry = "VeyraVideoEnhance";
constexpr int kMaxMessage = 65536;
class MinimizeNativeFilter final : public QAbstractNativeEventFilter {
public:
    std::function<bool(quintptr)> minimizeOwnedWindow;
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override {
#ifdef Q_OS_WIN
        if (eventType != QByteArrayLiteral("windows_generic_MSG") && eventType != QByteArrayLiteral("windows_dispatcher_MSG")) return false;
        const auto* native = static_cast<const MSG*>(message);
        if (native && native->message == WM_SYSCOMMAND && (native->wParam & 0xFFF0u) == SC_MINIMIZE &&
            minimizeOwnedWindow && minimizeOwnedWindow(reinterpret_cast<quintptr>(native->hwnd))) {
            if (result) *result = 0;
            return true;
        }
#else
        Q_UNUSED(eventType); Q_UNUSED(message); Q_UNUSED(result);
#endif
        return false;
    }
};

bool nativeTrayRegistered(const QSystemTrayIcon* tray) {
    if (!tray || tray->icon().isNull()) return false;
#ifdef Q_OS_WIN
    // Qt 6.8.3's isSystemTrayAvailable() always returns true on Windows,
    // while isVisible() only reflects a requested state. Query the real shell
    // registration for the unique Qt tray helper on this GUI thread. Qt owns
    // registration/recovery; this helper is read-only. If a future Qt changes
    // its helper identity, fail open by keeping the main window accessible.
    struct Search { HWND window = nullptr; unsigned count = 0; } search;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM context) -> BOOL {
        wchar_t title[256]{}, cls[256]{};
        GetWindowTextW(window, title, 256);
        GetClassNameW(window, cls, 256);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() && wcscmp(title, L"QTrayIconMessageWindow") == 0 &&
            wcsstr(cls, L"TrayIconMessageWindowClass")) {
            auto& result = *reinterpret_cast<Search*>(context);
            result.window = window;
            ++result.count;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (search.count != 1) return false;
    NOTIFYICONIDENTIFIER id{};
    id.cbSize = sizeof(id);
    id.hWnd = search.window;
    id.uID = 0; // fixed by the pinned Qt Windows tray implementation
    RECT rectangle{};
    // Folded overflow icons can return S_FALSE or a rectangle off-screen.
    // Both are registered icons; Qt's public geometry() filters them out.
    return SUCCEEDED(Shell_NotifyIconGetRect(&id, &rectangle));
#else
    return QSystemTrayIcon::isSystemTrayAvailable() && tray->isVisible();
#endif
}
QString userIdentity() {
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    QByteArray buffer(int(bytes), Qt::Uninitialized);
    const bool valid = bytes && GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes);
    LPWSTR sid = nullptr;
    QString result;
    if (valid && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
        result = QString::fromWCharArray(sid);
        LocalFree(sid);
    }
    CloseHandle(token);
    return result;
#else
    return qEnvironmentVariable("USER");
#endif
}
QString localFile(const QString& path) {
    if (path.isEmpty() || path.size() > 30000 || path.contains(QChar(0)) ||
        path.contains(QLatin1Char('\n')) || path.contains(QLatin1Char('\r')) ||
        path.startsWith(QStringLiteral("\\\\")) || path.startsWith(QStringLiteral("//")) ||
        path.contains(QStringLiteral("://"))) return {};
    const QFileInfo file(path);
    if (!file.isFile()) return {};
    const QString canonical = file.canonicalFilePath();
    if (canonical.startsWith(QStringLiteral("//")) || canonical.startsWith(QStringLiteral("\\\\"))) return {};
    return canonical;
}
QByteArray launchMessage(const QStringList& args) {
    const QString path = DesktopIntegration::explicitLocalOpenFile(args);
    QJsonObject object{{"schema", 1}, {"command", path.isEmpty() ? (args.contains(QStringLiteral("--tray")) ? "ping" : "show") : "open-file"}};
    if (!path.isEmpty()) object.insert("path", path);
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}
} // namespace

struct DesktopIntegration::Impl {
    QString directory, preferencesFile, registryPath, error;
    Options options;
    bool closeToTray = true, minimizeToTray = true, browserWanted = false, browserActual = false;
    bool quitting = false, registered = false, pendingShow = false;
    bool minimizePending = false, hiddenByMinimize = false;
    QWindow::Visibility restoreVisibility = QWindow::Windowed;
    QString pendingOpen;
    QPointer<QWindow> window;
    std::unique_ptr<QLockFile> lock;
    QLocalServer server;
    QSet<QLocalSocket*> clients;
    std::unique_ptr<QMenu> menu;
    std::unique_ptr<QSystemTrayIcon> tray;
    QAction* browserAction = nullptr;
    QAction* startupAction = nullptr;
    QTimer registrationTimer;
    std::unique_ptr<MinimizeNativeFilter> nativeMinimizeFilter;
};

DesktopIntegration::DesktopIntegration(QString directory, Options options, QObject* parent)
    : QObject(parent), p_(std::make_unique<Impl>()) {
    p_->directory = QDir(directory).absolutePath();
    p_->preferencesFile = p_->directory + QStringLiteral("/desktop-preferences.v1.json");
    p_->options = std::move(options);
    p_->registryPath = p_->options.startupRegistryPath.isEmpty()
        ? QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run")
        : p_->options.startupRegistryPath;
    QFile file(p_->preferencesFile);
    if (file.open(QIODevice::ReadOnly)) {
        const auto object = QJsonDocument::fromJson(file.read(65537)).object();
        if (object.value("schema").toInt() == 1) {
            if (object.value("closeToTray").isBool()) p_->closeToTray = object.value("closeToTray").toBool();
            if (object.value("minimizeToTray").isBool()) p_->minimizeToTray = object.value("minimizeToTray").toBool();
            if (object.value("browserWanted").isBool()) p_->browserWanted = object.value("browserWanted").toBool();
        }
    }
    connect(qApp, &QCoreApplication::aboutToQuit, this, &DesktopIntegration::prepareSessionExit);
    connect(qApp, &QGuiApplication::commitDataRequest, this,
        [this](QSessionManager&) { prepareSessionExit(); }, Qt::DirectConnection);
    connect(&p_->registrationTimer, &QTimer::timeout, this, &DesktopIntegration::checkTrayRegistration);
}
DesktopIntegration::~DesktopIntegration() {
    if (p_->nativeMinimizeFilter && QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(p_->nativeMinimizeFilter.get());
    p_->registrationTimer.stop();
    if (p_->window) p_->window->removeEventFilter(this);
    if (p_->tray) p_->tray->hide();
    p_->server.close();
    const auto clients = p_->clients;
    for (auto* socket : clients) delete socket;
}

QString DesktopIntegration::explicitLocalOpenFile(const QStringList& args) {
    const QStringList valueOptions{QStringLiteral("--data-dir"), QStringLiteral("--import-1.4.4-data"),
        QStringLiteral("--page"), QStringLiteral("--tab"), QStringLiteral("--dialog"),
        QStringLiteral("--menu"), QStringLiteral("--aspect"), QStringLiteral("--tip"),
        QStringLiteral("--export-out"), QStringLiteral("--full-bar"), QStringLiteral("--size"),
        QStringLiteral("--motion-probe"), QStringLiteral("--exit-after"), QStringLiteral("--video-probe"),
        QStringLiteral("--slow-animations"), QStringLiteral("--native-preset")};
    QString candidate;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--open") && i + 1 < args.size()) candidate = args[++i];
        else if (valueOptions.contains(args[i]) && i + 1 < args.size()) ++i;
        else if (!args[i].startsWith(QLatin1Char('-'))) candidate = args[i];
    }
    return localFile(candidate);
}

DesktopIntegration::InstanceResult DesktopIntegration::acquireInstance(const QStringList& args) {
    if (p_->options.localPlayer) {
        qInfo() << "desktop: independent local player; no resident IPC lock";
        return InstanceResult::Primary;
    }
    const QString user = userIdentity();
    if (user.isEmpty()) { p_->error = QStringLiteral("Cannot determine the current user"); return InstanceResult::Failed; }
    const QString scope = user + QLatin1Char('|') + p_->options.instanceScope;
    const auto hash = QCryptographicHash::hash(scope.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    const QString name = QStringLiteral("VeyraDesktop-v1-") + QString::fromLatin1(hash);
    const QString lockDirectory = p_->options.instanceDirectory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Veyra/desktop")
        : p_->options.instanceDirectory;
    if (!QDir().mkpath(lockDirectory)) { p_->error = QStringLiteral("Cannot create the user instance directory"); return InstanceResult::Failed; }
    p_->lock = std::make_unique<QLockFile>(lockDirectory + QLatin1Char('/') + name + QStringLiteral(".lock"));
    p_->lock->setStaleLockTime(0); // a live long-running app is never stale
    if (!p_->lock->tryLock(0)) {
        QElapsedTimer deadline; deadline.start();
        while (deadline.elapsed() < 3000) {
            QLocalSocket socket;
            socket.connectToServer(name);
            if (socket.waitForConnected(150)) {
                const auto message = launchMessage(args);
                if (message.size() <= kMaxMessage && socket.write(message) == message.size() && socket.waitForBytesWritten(500)) {
                    socket.disconnectFromServer();
                    qInfo() << "desktop: secondary launch forwarded; launch flags are not transported";
                    return InstanceResult::Forwarded;
                }
            }
            QThread::msleep(25);
        }
        p_->error = QStringLiteral("The existing Veyra instance did not respond");
        return InstanceResult::Failed;
    }
    p_->server.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(name); // only under the current user's exclusive lock
    if (!p_->server.listen(name)) { p_->error = p_->server.errorString(); return InstanceResult::Failed; }
    connect(&p_->server, &QLocalServer::newConnection, this, [this] {
        while (auto* socket = p_->server.nextPendingConnection()) {
            socket->setParent(this);
            if (p_->clients.size() >= 16) { socket->abort(); socket->deleteLater(); continue; }
            p_->clients.insert(socket);
            connect(socket, &QObject::destroyed, this, [this, socket] { p_->clients.remove(socket); });
            socket->setReadBufferSize(kMaxMessage + 1);
            auto data = std::make_shared<QByteArray>();
            auto handled = std::make_shared<bool>(false);
            const auto read = [this, socket, data, handled] {
                if (*handled) return;
                data->append(socket->readAll());
                if (data->size() > kMaxMessage) { *handled = true; socket->abort(); return; }
                if (!data->endsWith('\n')) return;
                *handled = true;
                const auto doc = QJsonDocument::fromJson(data->trimmed());
                const auto object = doc.object();
                const auto command = object.value("command").toString();
                const bool envelope = doc.isObject() && object.value("schema").isDouble() && object.value("schema").toDouble() == 1;
                if (envelope && command == QLatin1String("ping") && object.size() == 2) {
                    // A duplicate login launch must not raise the resident window.
                } else if (envelope && command == QLatin1String("show") && object.size() == 2) {
                    showMainWindow();
                } else if (envelope && command == QLatin1String("open-file") && object.size() == 3 && object.value("path").isString()) {
                    const auto path = object.value("path").toString();
                    const auto canonical = localFile(path);
                    if (QFileInfo(path).isAbsolute() && !canonical.isEmpty() && canonical == path) {
                        if (p_->options.residentBrowser) launchLocalPlayer(canonical);
                        else if (p_->window) emit openFileRequested(canonical);
                        else p_->pendingOpen = canonical;
                    }
                }
                socket->disconnectFromServer();
            };
            connect(socket, &QLocalSocket::readyRead, this, read);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(2000, socket, [socket] { socket->abort(); socket->deleteLater(); });
            read();
        }
    });
    qInfo() << "desktop: primary user instance acquired before player creation";
    return InstanceResult::Primary;
}

QString DesktopIntegration::startupCommand(const QString& executable, const QString& directory) {
    const QString exe = QDir::toNativeSeparators(QFileInfo(executable).absoluteFilePath());
    const QString data = QDir::toNativeSeparators(QDir(directory).absolutePath());
    if (exe.contains(QLatin1Char('"')) || data.contains(QLatin1Char('"')) ||
        exe.contains(QLatin1Char('\n')) || data.contains(QLatin1Char('\n')) ||
        exe.contains(QLatin1Char('\r')) || data.contains(QLatin1Char('\r'))) return {};
    return QStringLiteral("\"%1\" --data-dir \"%2\" --tray").arg(exe, data);
}
bool DesktopIntegration::loginStartup() const {
#ifdef Q_OS_WIN
    QSettings settings(p_->registryPath, QSettings::NativeFormat);
    const auto command = startupCommand(QCoreApplication::applicationFilePath(), p_->directory);
    return !command.isEmpty() && settings.value(QString::fromLatin1(kRunEntry)).toString() == command;
#else
    return false;
#endif
}
void DesktopIntegration::setLoginStartup(bool enabled) {
    if (localPlayer()) return;
#ifdef Q_OS_WIN
    QSettings settings(p_->registryPath, QSettings::NativeFormat);
    const auto entry = QString::fromLatin1(kRunEntry);
    const auto command = startupCommand(QCoreApplication::applicationFilePath(), p_->directory);
    if (enabled && command.isEmpty()) { emit notice(tr("无法保存登录启动设置"), true); return; }
    if (enabled) settings.setValue(entry, command);
    else settings.remove(entry); // this application's dedicated value only
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        p_->error = QStringLiteral("Cannot update Veyra's user startup entry");
        emit notice(tr("无法保存登录启动设置"), true);
    }
#else
    Q_UNUSED(enabled);
#endif
    if (p_->startupAction) p_->startupAction->setChecked(loginStartup());
    emit loginStartupChanged();
}
bool DesktopIntegration::configureDesktop() {
    setLoginStartup(true);
    if (!loginStartup()) return false;
    p_->closeToTray = true;
    p_->browserWanted = true;
    return savePreferences();
}
bool DesktopIntegration::localPlayer() const { return p_->options.localPlayer; }
bool DesktopIntegration::closeToTray() const { return !localPlayer() && (p_->options.residentBrowser || p_->closeToTray); }
bool DesktopIntegration::minimizeToTray() const { return !localPlayer() && p_->minimizeToTray; }
bool DesktopIntegration::browserWanted() const { return p_->browserWanted; }
bool DesktopIntegration::startHidden() const { return !localPlayer() && (p_->options.trayLaunch || p_->options.backgroundProbe); }
bool DesktopIntegration::trayRegistered() const { return p_->registered; }
QString DesktopIntegration::errorString() const { return p_->error; }
QString DesktopIntegration::instanceServerName() const { return p_->server.serverName(); }
bool DesktopIntegration::savePreferences() {
    if (localPlayer()) return true; // local windows never write resident preferences
    if (!QDir().mkpath(p_->directory)) { p_->error = QStringLiteral("Cannot create the desktop preference directory"); return false; }
    QSaveFile file(p_->preferencesFile);
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"closeToTray", p_->closeToTray}, {"minimizeToTray", p_->minimizeToTray},
        {"browserWanted", p_->browserWanted}}).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        p_->error = file.errorString();
        emit notice(tr("无法保存桌面设置"), true);
        return false;
    }
    emit preferencesChanged();
    return true;
}
void DesktopIntegration::setCloseToTray(bool enabled) {
    if (p_->closeToTray == enabled) return;
    p_->closeToTray = enabled;
    savePreferences();
}
void DesktopIntegration::setMinimizeToTray(bool enabled) {
    if (p_->minimizeToTray == enabled) return;
    p_->minimizeToTray = enabled;
    savePreferences();
}
void DesktopIntegration::rememberBrowserUserChoice(bool enabled) {
    if (localPlayer()) return;
    // Actual fail-closed/source-switch/shutdown transitions never call this.
    if (p_->browserWanted == enabled) return;
    p_->browserWanted = enabled;
    savePreferences();
    qInfo() << "desktop: saved explicit browser preference" << enabled;
}
void DesktopIntegration::requestBrowserEnabled(bool enabled) {
    if (localPlayer()) { showResidentWindow(); return; }
    rememberBrowserUserChoice(enabled);
    emit browserEnableRequested(enabled);
}
void DesktopIntegration::setBrowserActual(bool enabled) {
    p_->browserActual = enabled;
    if (p_->browserAction) p_->browserAction->setChecked(enabled);
    if (p_->tray) p_->tray->setToolTip(enabled ? tr("Veyra 视频增强 · 浏览器增强已开启") : tr("Veyra 视频增强 · 浏览器增强已关闭"));
}

void DesktopIntegration::initializeTray() {
    if (p_->options.backgroundProbe || localPlayer()) return; // only the resident owns a tray icon
    p_->menu = std::make_unique<QMenu>();
    p_->menu->setObjectName(QStringLiteral("VeyraDesktopMenu"));
    auto* show = p_->menu->addAction(tr("显示主界面"));
    connect(show, &QAction::triggered, this, &DesktopIntegration::showMainWindow);
    p_->browserAction = p_->menu->addAction(tr("浏览器增强"));
    p_->browserAction->setCheckable(true);
    connect(p_->browserAction, &QAction::triggered, this, &DesktopIntegration::requestBrowserEnabled);
    p_->startupAction = p_->menu->addAction(tr("登录时启动"));
    p_->startupAction->setCheckable(true);
    p_->startupAction->setChecked(loginStartup());
    connect(p_->startupAction, &QAction::triggered, this, &DesktopIntegration::setLoginStartup);
    p_->menu->addSeparator();
    auto* exit = p_->menu->addAction(tr("退出"));
    exit->setObjectName(QStringLiteral("desktop-quit"));
    connect(exit, &QAction::triggered, this, &DesktopIntegration::requestExit);
    p_->tray = std::make_unique<QSystemTrayIcon>(qApp->windowIcon());
    p_->tray->setContextMenu(p_->menu.get());
    connect(p_->tray.get(), &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) showMainWindow();
    });
    p_->tray->show();
    setBrowserActual(p_->browserActual);
    checkTrayRegistration();
    p_->registrationTimer.start(5000); // accessibility fallback after shell recovery/failure
}
void DesktopIntegration::checkTrayRegistration() {
    const bool registered = p_->options.trayRegistrationProbe
        ? p_->options.trayRegistrationProbe()
        : nativeTrayRegistered(p_->tray.get());
    if (registered != p_->registered) {
        p_->registered = registered;
        qInfo() << "desktop: trayRegistered=" << registered;
        emit trayRegistrationChanged();
    }
    if (!registered && p_->window && !p_->window->isVisible() && !p_->quitting) {
        qWarning() << "desktop: tray registration unavailable; showing main window";
        p_->hiddenByMinimize = false;
        p_->window->show(); // preserve access without requesting foreground focus
    }
}
void DesktopIntegration::attachWindow(QWindow* window) {
    p_->window = window;
    if (window->visibility() == QWindow::Windowed || window->visibility() == QWindow::Maximized ||
        window->visibility() == QWindow::FullScreen) p_->restoreVisibility = window->visibility();
    window->installEventFilter(this);
    if (!p_->nativeMinimizeFilter) {
        p_->nativeMinimizeFilter = std::make_unique<MinimizeNativeFilter>();
        p_->nativeMinimizeFilter->minimizeOwnedWindow = [this](quintptr handle) {
            // Observe only this integration's existing native owner. A command
            // for an owned tool window or another application is never consumed.
            if (!p_->window || !p_->window->handle() || quintptr(p_->window->winId()) != handle) return false;
            return hideForMinimize();
        };
        QCoreApplication::instance()->installNativeEventFilter(p_->nativeMinimizeFilter.get());
    }
    const QPointer<QWindow> watched(window);
    connect(window, &QWindow::visibilityChanged, this, [this, watched](QWindow::Visibility visibility) {
        if (!watched || p_->window != watched) return;
        qInfo() << "desktop-state: visibility" << int(visibility) << "state=" << int(watched->windowStates())
            << "restoreBefore=" << int(p_->restoreVisibility) << "hiddenByMinimize=" << p_->hiddenByMinimize;
        if (!p_->hiddenByMinimize && (visibility == QWindow::Windowed || visibility == QWindow::Maximized || visibility == QWindow::FullScreen))
            p_->restoreVisibility = visibility;
        if (visibility == QWindow::Minimized) queueMinimizeToTray();
    });
    qApp->setQuitOnLastWindowClosed(false);
    initializeTray();
    qInfo() << "desktop: ready startHidden=" << startHidden() << "loginStartup=" << loginStartup()
        << "closeToTray=" << closeToTray() << "minimizeToTray=" << minimizeToTray() << "browserWanted=" << browserWanted()
        << "trayRegistered=" << trayRegistered();
    if (p_->pendingShow) showMainWindow();
    if (!p_->pendingOpen.isEmpty()) {
        const auto path = p_->pendingOpen;
        p_->pendingOpen.clear();
        QTimer::singleShot(0, this, [this, path] { emit openFileRequested(path); });
    }
}
bool DesktopIntegration::launchLocalPlayer(const QString& path) {
    const auto canonical = localFile(path);
    if (canonical.isEmpty()) { emit notice(tr("无法打开本地文件"), true); return false; }
    const bool ok = QProcess::startDetached(QCoreApplication::applicationFilePath(),
        {QStringLiteral("--local-player"), QStringLiteral("--data-dir"), p_->directory,
         QStringLiteral("--open"), canonical}, QCoreApplication::applicationDirPath());
    if (!ok) emit notice(tr("无法启动独立播放器"), true);
    return ok;
}
void DesktopIntegration::showResidentWindow() {
    if (!localPlayer()) { showMainWindow(); return; }
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
        {QStringLiteral("--data-dir"), p_->directory}, QCoreApplication::applicationDirPath()))
        emit notice(tr("无法打开浏览器增强主界面"), true);
}
void DesktopIntegration::showMainWindow() {
    qInfo() << "desktop-state: restore requested visibility=" << (p_->window ? int(p_->window->visibility()) : -1)
        << "state=" << (p_->window ? int(p_->window->windowStates()) : -1)
        << "target=" << int(p_->restoreVisibility) << "hiddenByMinimize=" << p_->hiddenByMinimize;
    emit showRequested();
    if (!p_->window) { p_->pendingShow = true; return; }
    p_->pendingShow = false;
    if (p_->hiddenByMinimize || p_->window->visibility() == QWindow::Minimized) {
        // Native show calls can synchronously emit intermediate state changes.
        // Keep the selected target on the stack while restoring it.
        const auto restoreVisibility = p_->restoreVisibility;
        p_->hiddenByMinimize = false;
        if (restoreVisibility == QWindow::Maximized) p_->window->showMaximized();
        else if (restoreVisibility == QWindow::FullScreen) p_->window->showFullScreen();
        else p_->window->showNormal();
    } else p_->window->show();
    p_->window->raise();
    p_->window->requestActivate();
}
bool DesktopIntegration::hideForMinimize() {
    if (!p_->window || !minimizeToTray() || p_->quitting || p_->options.timedExit || p_->options.backgroundProbe) return false;
    const bool registered = p_->options.trayRegistrationProbe
        ? p_->options.trayRegistrationProbe() : nativeTrayRegistered(p_->tray.get());
    if (p_->registered != registered) { p_->registered = registered; emit trayRegistrationChanged(); }
    if (!registered) return false;
    const auto visibility = p_->window->visibility();
    if (!p_->hiddenByMinimize && (visibility == QWindow::Windowed || visibility == QWindow::Maximized || visibility == QWindow::FullScreen))
        p_->restoreVisibility = visibility;
    // Capture the user's actual state before Qt leaves native fullscreen. Hiding
    // the owner directly avoids a fullscreen -> normal -> minimized transition.
    p_->hiddenByMinimize = true;
    qInfo() << "desktop-state: minimize intent hide visibility=" << int(visibility)
        << "state=" << int(p_->window->windowStates()) << "target=" << int(p_->restoreVisibility);
    p_->window->hide();
    return true;
}
void DesktopIntegration::minimizeWindow() {
    if (!p_->window || p_->quitting) return;
    if (!hideForMinimize()) p_->window->showMinimized();
}
void DesktopIntegration::queueMinimizeToTray() {
    if (p_->minimizePending || !p_->window || !p_->minimizeToTray || p_->quitting ||
        p_->options.timedExit || p_->options.backgroundProbe) return;
    p_->minimizePending = true;
    const QPointer<QWindow> window(p_->window);
    // Let Qt finish its native state transition before removing the window from
    // the taskbar. A rapid restore, user option change or quit cancels this work.
    QTimer::singleShot(0, this, [this, window] {
        p_->minimizePending = false;
        if (!window || p_->window != window || window->visibility() != QWindow::Minimized ||
            !p_->minimizeToTray || p_->quitting || p_->options.timedExit || p_->options.backgroundProbe) return;
        // External Qt callers may still enter the native Minimized state.
        // Preserve their normal taskbar fallback if no tray is reachable.
        if (hideForMinimize()) qInfo() << "desktop: minimized main window to tray";
    });
}
void DesktopIntegration::prepareSessionExit() { p_->quitting = true; }
void DesktopIntegration::requestExit() {
    prepareSessionExit();
    qInfo() << "desktop: explicit exit; returning through the original player teardown";
    QCoreApplication::quit();
}
bool DesktopIntegration::eventFilter(QObject* watched, QEvent* event) {
    if (watched != p_->window.data()) return false;
    if (event->type() == QEvent::WindowStateChange) {
        const auto oldState = static_cast<QWindowStateChangeEvent*>(event)->oldState();
        qInfo() << "desktop-state: stateChange old=" << int(oldState) << "new=" << int(p_->window->windowStates())
            << "visibility=" << int(p_->window->visibility()) << "restoreBefore=" << int(p_->restoreVisibility);
        if (!p_->hiddenByMinimize && p_->window->windowStates().testFlag(Qt::WindowMinimized) && !oldState.testFlag(Qt::WindowMinimized)) {
            if (oldState.testFlag(Qt::WindowFullScreen)) p_->restoreVisibility = QWindow::FullScreen;
            else if (oldState.testFlag(Qt::WindowMaximized)) p_->restoreVisibility = QWindow::Maximized;
            else p_->restoreVisibility = QWindow::Windowed;
        }
        queueMinimizeToTray();
        return false;
    }
    if (event->type() != QEvent::Close) return false;
    if (!p_->quitting && !p_->options.timedExit && !p_->options.backgroundProbe && closeToTray()) {
        checkTrayRegistration();
        if (p_->registered) {
            static_cast<QCloseEvent*>(event)->ignore();
            emit closingToTray();
            p_->window->hide();
            qInfo() << "desktop: main window closed to tray";
            return true;
        }
    }
    requestExit();
    return false;
}
} // namespace veyra::ui
