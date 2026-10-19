#include "veyra/ui/DesktopIntegration.h"
#include <QApplication>
#include <QAction>
#include <QMenu>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLocalSocket>
#include <QPixmap>
#include <QProcess>
#include <QSettings>
#include <QTimer>
#include <QWindow>
#include <iostream>

using veyra::ui::DesktopIntegration;
namespace {
int assertions = 0;
bool check(bool condition, const char* why) {
    ++assertions;
    if (!condition) std::cerr << "FAIL: " << why << '\n';
    return condition;
}
bool waitUntil(const std::function<bool()>& ready, int timeoutMs = 1500) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return ready();
}
void drainEvents(int milliseconds = 80) {
    QElapsedTimer timer; timer.start();
    waitUntil([&] { return timer.elapsed() >= milliseconds; }, milliseconds + 1000);
}
DesktopIntegration::Options options(const QString& root) {
    DesktopIntegration::Options value;
    value.instanceDirectory = root + QStringLiteral("/locks");
    value.instanceScope = root; // cannot be supplied to the product CLI
    const auto hash = QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex();
    value.startupRegistryPath = QStringLiteral("HKEY_CURRENT_USER\\Software\\Veyra\\DesktopIntegrationTests\\") + QString::fromLatin1(hash);
    value.trayRegistrationProbe = [] { return true; };
    return value;
}
bool child(const QString& mode, const QString& root, const QStringList& tail = {}) {
    QProcess process;
    process.start(QCoreApplication::applicationFilePath(), QStringList{mode, root} + tail);
    if (!process.waitForStarted(1000)) return false;
    QElapsedTimer limit; limit.start();
    while (process.state() != QProcess::NotRunning && limit.elapsed() < 5000) {
        QCoreApplication::processEvents();
        process.waitForFinished(10);
    }
    const bool done = process.state() == QProcess::NotRunning;
    if (!done) { process.kill(); process.waitForFinished(1000); }
    if (!done || process.exitCode() != 0) std::cerr << process.readAllStandardError().toStdString();
    QCoreApplication::processEvents();
    return done && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
void sendRaw(const QString& server, const QByteArray& bytes) {
    QLocalSocket socket;
    socket.connectToServer(server);
    if (socket.waitForConnected(500)) {
        socket.write(bytes);
        socket.waitForBytesWritten(500);
        QCoreApplication::processEvents();
        socket.disconnectFromServer();
        QCoreApplication::processEvents();
    }
}
}
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QPixmap pixmap(16, 16); pixmap.fill(Qt::blue); app.setWindowIcon(QIcon(pixmap));
    const auto args = app.arguments();
    if (args.size() < 2) { std::cerr << "usage: veyra_desktop_tests <new scratch directory>\n"; return 2; }
    if (args[1].startsWith(QStringLiteral("--"))) {
        if (args.size() < 3) return 2;
        const QString root = args[2];
        auto opts = options(root);
        if (args[1] == QLatin1String("--secondary") || args[1] == QLatin1String("--open-file")) {
            DesktopIntegration desktop(root + QStringLiteral("/profile"), opts);
            QStringList launch{QStringLiteral("veyra"), QStringLiteral("--data-dir"), root + QStringLiteral("/sample.txt"),
                QStringLiteral("--exit-after"), QStringLiteral("1"), QStringLiteral("--browser-enable"),
                QStringLiteral("--native-preset"), root + QStringLiteral("/sample.txt")};
            if (args[1] == QLatin1String("--open-file")) launch << QStringLiteral("--open") << root + QStringLiteral("/sample.txt");
            return desktop.acquireInstance(launch) == DesktopIntegration::InstanceResult::Forwarded ? 0 : 1;
        }
        opts.timedExit = args[1] == QLatin1String("--timed");
        opts.localPlayer = args[1] == QLatin1String("--local-close");
        DesktopIntegration desktop(root + QStringLiteral("/profile"), opts);
        QWindow window;
        desktop.attachWindow(&window); window.show();
        if (args[1] == QLatin1String("--session-exit")) desktop.prepareSessionExit();
        if (args[1] == QLatin1String("--close-exit")) desktop.setCloseToTray(false);
        QTimer::singleShot(25, &window, [&window, args] {
            if (args[1] == QLatin1String("--menu-exit")) {
                for (auto* widget : QApplication::topLevelWidgets()) {
                    if (widget->objectName() == QLatin1String("VeyraDesktopMenu")) {
                        if (auto* exit = widget->findChild<QAction*>(QStringLiteral("desktop-quit"))) exit->trigger();
                    }
                }
            } else window.close();
        });
        QTimer::singleShot(2000, &app, [] { QCoreApplication::exit(3); });
        return app.exec();
    }
    const QString root = QFileInfo(args[1]).absoluteFilePath();
    if (QFileInfo::exists(root) || !QDir().mkpath(root)) { std::cerr << "scratch directory must be new\n"; return 2; }
    QFile sample(root + QStringLiteral("/sample.txt"));
    if (!sample.open(QIODevice::WriteOnly)) return 2;
    sample.write("test local file"); sample.close();
    const QString profile = root + QStringLiteral("/profile");
    auto opts = options(root);
    bool ok = true;
    {
        DesktopIntegration desktop(profile, opts);
        ok &= check(!desktop.browserWanted(), "first launch defaults OFF");
        desktop.rememberBrowserUserChoice(true);
        desktop.setBrowserActual(false); // internal failure/source switch/shutdown
    }
    {
        DesktopIntegration restored(profile, opts);
        ok &= check(restored.browserWanted(), "internal OFF never overwrites the saved user ON");
        restored.rememberBrowserUserChoice(false); // explicit OFF while already actually OFF
    }
    {
        DesktopIntegration desktop(profile, opts);
        ok &= check(!desktop.browserWanted(), "explicit OFF survives a new process object");
        ok &= check(desktop.acquireInstance({QStringLiteral("veyra")}) == DesktopIntegration::InstanceResult::Primary, "primary owns user lock and IPC");
        QWindow window; window.show(); desktop.attachWindow(&window);
        int shows = 0, browserRequests = 0, opens = 0;
        QString opened;
        QObject::connect(&desktop, &DesktopIntegration::showRequested, &app, [&] { ++shows; });
        QObject::connect(&desktop, &DesktopIntegration::browserEnableRequested, &app, [&](bool) { ++browserRequests; });
        QObject::connect(&desktop, &DesktopIntegration::openFileRequested, &app, [&](const QString& path) { ++opens; opened = path; });
        window.close();
        ok &= check(!window.isVisible() && desktop.trayRegistered(), "ordinary close hides a reachable window");
        ok &= check(child(QStringLiteral("--secondary"), root), "second process forwards ordinary launch");
        ok &= check(window.isVisible() && shows == 1 && opens == 0 && browserRequests == 0 && !desktop.browserWanted(),
            "duplicate launch shows the same owner without changing OFF or forwarding test arguments");
        sendRaw(desktop.instanceServerName(), "{\"schema\":1,\"command\":\"show\",\"exitAfter\":1}\n");
        sendRaw(desktop.instanceServerName(), "{\"schema\":1,\"command\":\"open-file\",\"path\":\"https://example.com/video\"}\n");
        sendRaw(desktop.instanceServerName(), QByteArray(65537, 'x'));
        ok &= check(shows == 1 && opens == 0 && !desktop.browserWanted(), "extra fields, URL and oversized IPC have no authority");
        ok &= check(child(QStringLiteral("--open-file"), root), "explicit local file forwards");
        ok &= check(opens == 1 && opened == QFileInfo(sample.fileName()).canonicalFilePath() && !desktop.browserWanted(),
            "local file uses the owner's normal open callback and keeps browser preference");
        desktop.setBrowserActual(true);
        ok &= check(!desktop.browserWanted(), "actual runtime ON does not replace saved OFF");
        QSettings registry(opts.startupRegistryPath, QSettings::NativeFormat);
        registry.setValue(QStringLiteral("Unrelated"), QStringLiteral("preserve")); registry.sync();
        desktop.setLoginStartup(true);
        ok &= check(desktop.loginStartup(), "isolated user Run value saves quoted absolute launch command");
        registry.sync();
        const auto command = registry.value(QStringLiteral("VeyraVideoEnhance")).toString();
        ok &= check(command.endsWith(QStringLiteral(" --tray")) && !command.contains(QStringLiteral("browser-enable")) &&
            !command.contains(QStringLiteral("exit-after")), "daily startup contains no forced ON or test exit");
        desktop.setLoginStartup(false);
        registry.sync();
        ok &= check(!desktop.loginStartup() && registry.value(QStringLiteral("Unrelated")).toString() == QLatin1String("preserve"),
            "startup OFF removes only Veyra's dedicated value");
        registry.remove(QString()); registry.sync(); // exclusively test-owned branch
    }
    {
        auto unavailable = opts; unavailable.trayLaunch = true; unavailable.trayRegistrationProbe = [] { return false; };
        DesktopIntegration desktop(profile, unavailable);
        QWindow window; desktop.attachWindow(&window);
        ok &= check(window.isVisible() && !desktop.trayRegistered(), "failed tray registration shows the hidden login window");
        ok &= check(!desktop.browserWanted(), "failed registration does not change user OFF");
    }
    {
        const QString legacy = root + QStringLiteral("/legacy-schema1");
        QDir().mkpath(legacy);
        QFile file(legacy + QStringLiteral("/desktop-preferences.v1.json"));
        if (!file.open(QIODevice::WriteOnly)) return 2;
        file.write("{\"schema\":1,\"closeToTray\":false,\"browserWanted\":true}"); file.close();
        {
            DesktopIntegration desktop(legacy, opts);
            ok &= check(desktop.minimizeToTray(), "legacy schema1 missing minimize preference defaults ON");
            ok &= check(!desktop.closeToTray() && desktop.browserWanted(), "legacy close and browser preferences remain independent");
            desktop.setMinimizeToTray(false);
            ok &= check(!desktop.minimizeToTray() && !desktop.closeToTray() && desktop.browserWanted(), "minimize OFF never changes close-to-exit or browser ON");
        }
        {
            DesktopIntegration restored(legacy, opts);
            ok &= check(!restored.minimizeToTray() && !restored.closeToTray() && restored.browserWanted(), "all three independent preferences survive reload");
            restored.setMinimizeToTray(true);
            ok &= check(!restored.closeToTray() && restored.browserWanted(), "minimize ON never changes close-to-exit or browser ON");
        }
        if (!file.open(QIODevice::ReadOnly)) return 2;
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        ok &= check(object.value("schema").toInt() == 1 && object.value("minimizeToTray").toBool() &&
            !object.value("closeToTray").toBool() && object.value("browserWanted").toBool(), "serialization preserves schema1 and the requested independent values");
    }
    {
        DesktopIntegration desktop(root + QStringLiteral("/minimize-user-action"), opts);
        desktop.setCloseToTray(false); desktop.rememberBrowserUserChoice(true);
        QWindow window; window.resize(420, 240); desktop.attachWindow(&window);
        int nativeMinimizedTransitions = 0;
        QObject::connect(&window, &QWindow::windowStateChanged, &window, [&](Qt::WindowState state) {
            if (state == Qt::WindowMinimized) ++nativeMinimizedTransitions;
        });
        for (const auto visibility : {QWindow::Windowed, QWindow::Maximized, QWindow::FullScreen}) {
            window.setVisibility(visibility); drainEvents();
            nativeMinimizedTransitions = 0;
            desktop.minimizeWindow();
            ok &= check(window.visibility() == QWindow::Hidden && !window.isVisible(), "explicit minimize hides directly in every supported display mode");
            ok &= check(nativeMinimizedTransitions == 0, "tray user action avoids native minimized state and its fullscreen transitions");
            ok &= check(!desktop.closeToTray() && desktop.browserWanted(), "explicit minimize preserves close-to-exit and browser preference");
            desktop.showMainWindow(); drainEvents();
            ok &= check(window.visibility() == visibility && window.isVisible(), "explicit tray action restores the actual captured pre-hide mode");
        }
        window.showNormal(); drainEvents();
        desktop.setMinimizeToTray(false); desktop.minimizeWindow(); drainEvents();
        ok &= check(window.visibility() == QWindow::Minimized && window.isVisible(), "explicit minimize option OFF retains ordinary taskbar minimum");
    }
    {
        DesktopIntegration desktop(root + QStringLiteral("/minimize-policy"), opts);
        desktop.setCloseToTray(false); desktop.rememberBrowserUserChoice(true);
        QWindow window; window.resize(420, 240); desktop.attachWindow(&window); window.showNormal();
        ok &= check(waitUntil([&] { return window.visibility() == QWindow::Windowed; }), "real QWindow enters ordinary visible state");
        window.showMinimized();
        ok &= check(waitUntil([&] { return window.visibility() == QWindow::Hidden; }), "real minimize event hides window after queued tray check");
        ok &= check(!window.isVisible() && desktop.trayRegistered(), "minimized owner is hidden with reachable tray");
        ok &= check(!desktop.closeToTray() && desktop.browserWanted(), "minimizing preserves close-to-exit and browser ON");
        desktop.showMainWindow();
        ok &= check(waitUntil([&] { return window.isVisible() && window.visibility() == QWindow::Windowed; }), "tray restore shows ordinary window instead of re-minimizing");
        drainEvents();
        ok &= check(window.isVisible() && window.windowState() != Qt::WindowMinimized, "restored window remains visible after queued events");
        window.showMaximized();
        ok &= check(waitUntil([&] { return window.visibility() == QWindow::Maximized; }), "real QWindow enters maximized state");
        window.showMinimized();
        ok &= check(waitUntil([&] { return window.visibility() == QWindow::Hidden; }), "maximized window minimizes to tray");
        desktop.showMainWindow();
        ok &= check(waitUntil([&] { return window.visibility() == QWindow::Maximized && window.isVisible(); }), "tray restore preserves maximized state");
        window.showFullScreen();
        ok &= check(waitUntil([&] { return window.visibility() == QWindow::FullScreen; }), "real QWindow enters fullscreen state");
        window.showMinimized();
        ok &= check(waitUntil([&] { return window.visibility() == QWindow::Hidden; }), "fullscreen window minimizes to tray");
        desktop.showMainWindow();
        ok &= check(waitUntil([&] { return window.visibility() == QWindow::FullScreen && window.isVisible(); }), "tray restore preserves fullscreen state");
        window.showNormal(); desktop.setMinimizeToTray(false); window.showMinimized(); drainEvents();
        ok &= check(window.visibility() == QWindow::Minimized && window.isVisible(), "minimize preference OFF keeps ordinary minimized window available to taskbar");
        desktop.showMainWindow();
        ok &= check(waitUntil([&] { return window.visibility() == QWindow::Windowed; }), "ordinary minimized window can be restored");
        desktop.setMinimizeToTray(true); window.showMinimized(); window.showNormal(); drainEvents();
        ok &= check(window.isVisible() && window.visibility() == QWindow::Windowed, "rapid minimize then restore cancels queued hiding");
        window.showMinimized(); desktop.setMinimizeToTray(false); drainEvents();
        ok &= check(window.visibility() == QWindow::Minimized && window.isVisible(), "turning option OFF cancels already queued minimize work");
    }
    {
        bool registered = true;
        auto failureOptions = opts; failureOptions.trayRegistrationProbe = [&] { return registered; };
        DesktopIntegration desktop(root + QStringLiteral("/minimize-failed-tray"), failureOptions);
        QWindow window; desktop.attachWindow(&window); window.showNormal(); drainEvents();
        registered = false; window.showMinimized(); drainEvents();
        ok &= check(!desktop.trayRegistered() && window.isVisible() && window.visibility() == QWindow::Minimized,
            "failed real tray check leaves normal minimized state without showing or hiding");
        window.showNormal(); drainEvents(); desktop.minimizeWindow(); drainEvents();
        ok &= check(window.visibility() == QWindow::Minimized && window.isVisible(), "explicit minimize with failed tray falls back to taskbar safely");
    }
    {
        bool registered = true;
        auto recoveryOptions = opts; recoveryOptions.trayRegistrationProbe = [&] { return registered; };
        DesktopIntegration desktop(root + QStringLiteral("/minimize-tray-loss-recovery"), recoveryOptions);
        QWindow window; desktop.attachWindow(&window); window.showFullScreen(); drainEvents();
        desktop.minimizeWindow();
        ok &= check(window.visibility() == QWindow::Hidden && desktop.trayRegistered(), "tray loss case begins with fullscreen hidden to reachable tray");
        registered = false;
        ok &= check(waitUntil([&] { return window.isVisible(); }, 6500), "real five-second registration timer recovers hidden owner after tray loss");
        ok &= check(!desktop.trayRegistered(), "periodic recovery reports unavailable tray");
        window.showNormal(); drainEvents();
        registered = true; desktop.minimizeWindow();
        ok &= check(window.visibility() == QWindow::Hidden, "recovered window can minimize after tray becomes available");
        desktop.showMainWindow(); drainEvents();
        ok &= check(window.isVisible() && window.visibility() == QWindow::Windowed, "tray-loss fallback releases old fullscreen restore state");
    }
    for (int mode = 0; mode < 3; ++mode) {
        auto suppressed = opts;
        suppressed.timedExit = mode == 0; suppressed.backgroundProbe = mode == 1;
        DesktopIntegration desktop(root + QStringLiteral("/minimize-suppressed-%1").arg(mode), suppressed);
        QWindow window; desktop.attachWindow(&window); window.showNormal(); drainEvents();
        if (mode == 2) desktop.prepareSessionExit();
        window.showMinimized(); drainEvents();
        ok &= check(window.visibility() == QWindow::Minimized && window.isVisible(),
            "timed/background/session-exit mode never hides minimized owner");
        window.showNormal(); drainEvents(); desktop.minimizeWindow(); drainEvents();
        ok &= check(window.isVisible() && window.visibility() == (mode == 2 ? QWindow::Windowed : QWindow::Minimized),
            "explicit minimize never hides a timed/background/stopping owner");
    }
    {
        DesktopIntegration desktop(root + QStringLiteral("/minimize-deleted-owner"), opts);
        auto* window = new QWindow; desktop.attachWindow(window); window->showNormal();
        window->showMinimized(); delete window; drainEvents();
        ok &= check(desktop.minimizeToTray(), "queued callback safely ignores deleted QWindow owner");
    }
    {
        auto localOptions = opts;
        localOptions.localPlayer = true;
        localOptions.trayLaunch = true; // file opens must remain visible even with a tray flag
        DesktopIntegration first(profile, localOptions), second(profile, localOptions);
        ok &= check(first.localPlayer() && second.localPlayer(), "independent local role is explicit");
        ok &= check(first.acquireInstance({"veyra"}) == DesktopIntegration::InstanceResult::Primary &&
                    second.acquireInstance({"veyra"}) == DesktopIntegration::InstanceResult::Primary,
                    "two local players bypass the same resident lock");
        ok &= check(first.instanceServerName().isEmpty() && second.instanceServerName().isEmpty(),
                    "local players never host resident IPC");
        ok &= check(!first.closeToTray() && !first.minimizeToTray() && !first.startHidden(),
                    "local player closes to exit, minimizes normally and opens visibly");
        QFile beforeFile(profile + "/desktop-preferences.v1.json");
        beforeFile.open(QIODevice::ReadOnly); const auto before = beforeFile.readAll(); beforeFile.close();
        first.setCloseToTray(true); first.setMinimizeToTray(true); first.rememberBrowserUserChoice(true);
        first.setLoginStartup(true);
        QFile afterFile(profile + "/desktop-preferences.v1.json"); afterFile.open(QIODevice::ReadOnly);
        ok &= check(afterFile.readAll() == before, "local desktop changes never write resident preferences");
        ok &= check(!first.closeToTray() && !first.minimizeToTray(), "local role cannot be toggled into a tray process");
        QWindow window; first.attachWindow(&window); window.showNormal(); drainEvents();
        ok &= check(!first.trayRegistered(), "local player does not register a tray icon");
        first.minimizeWindow(); drainEvents();
        ok &= check(window.isVisible() && window.visibility() == QWindow::Minimized,
                    "local minimize stays in the normal taskbar");
    }
    {
        auto residentOptions = opts; residentOptions.residentBrowser = true;
        DesktopIntegration resident(root + "/resident-role", residentOptions);
        resident.setCloseToTray(false);
        ok &= check(!resident.localPlayer() && resident.closeToTray(), "resident close preserves its browser host");
    }
    ok &= check(child(QStringLiteral("--local-close"), root + QStringLiteral("/local-close")),
                "local close ends its own application loop even when saved close-to-tray is ON");
    ok &= check(child(QStringLiteral("--timed"), root + QStringLiteral("/timed")), "timed close exits even with close-to-tray ON");
    ok &= check(child(QStringLiteral("--session-exit"), root + QStringLiteral("/session")), "session ending exits even with close-to-tray ON");
    ok &= check(child(QStringLiteral("--menu-exit"), root + QStringLiteral("/menu-exit")), "tray menu Quit ends the application loop");
    ok &= check(child(QStringLiteral("--close-exit"), root + QStringLiteral("/close-exit")), "close-to-tray OFF restores closing as exit");
    std::cout << (ok ? "PASS" : "FAIL") << " desktop preferences, single instance, IPC, startup, minimize and exit policy; assertions=" << assertions << '\n';
    return ok ? 0 : 1;
}
