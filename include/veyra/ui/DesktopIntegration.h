#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>
#include <memory>

class QWindow;

namespace veyra::ui {

// Desktop lifecycle and preferences only. The player, browser endpoint and GPU
// remain owned by the existing bridge/engine. IPC never transports an argv.
class DesktopIntegration final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loginStartup READ loginStartup WRITE setLoginStartup NOTIFY loginStartupChanged)
    Q_PROPERTY(bool closeToTray READ closeToTray WRITE setCloseToTray NOTIFY preferencesChanged)
    Q_PROPERTY(bool minimizeToTray READ minimizeToTray WRITE setMinimizeToTray NOTIFY preferencesChanged)
    Q_PROPERTY(bool localPlayer READ localPlayer CONSTANT)
    Q_PROPERTY(bool browserWanted READ browserWanted NOTIFY preferencesChanged)
    Q_PROPERTY(bool startHidden READ startHidden CONSTANT)
    Q_PROPERTY(bool trayRegistered READ trayRegistered NOTIFY trayRegistrationChanged)
public:
    struct Options {
        bool localPlayer = false;
        bool residentBrowser = false;
        bool trayLaunch = false;
        bool timedExit = false;
        bool backgroundProbe = false;
        // C++ test seams, deliberately unavailable as command-line parameters.
        QString instanceDirectory;
        QString instanceScope;
        QString startupRegistryPath;
        std::function<bool()> trayRegistrationProbe;
    };
    enum class InstanceResult { Primary, Forwarded, Failed };

    explicit DesktopIntegration(QString dataDirectory, Options options, QObject* parent = nullptr);
    ~DesktopIntegration() override;
    InstanceResult acquireInstance(const QStringList& launchArguments);
    static QString explicitLocalOpenFile(const QStringList& arguments);
    static QString startupCommand(const QString& executable, const QString& dataDirectory);
    bool configureDesktop(); // one-time setup, no player or browser creation
    bool loginStartup() const;
    bool closeToTray() const;
    bool minimizeToTray() const;
    bool localPlayer() const;
    bool browserWanted() const;
    bool launchLocalPlayer(const QString& path);
    Q_INVOKABLE void showResidentWindow();
    bool startHidden() const;
    bool trayRegistered() const;
    QString errorString() const;
    QString instanceServerName() const;
    void attachWindow(QWindow* window);
    void setBrowserActual(bool enabled);
    void rememberBrowserUserChoice(bool enabled);
    void setLoginStartup(bool enabled);
    void setCloseToTray(bool enabled);
    void setMinimizeToTray(bool enabled);
    Q_INVOKABLE void showMainWindow();
    Q_INVOKABLE void minimizeWindow();
    Q_INVOKABLE void requestBrowserEnabled(bool enabled);
    Q_INVOKABLE void requestExit();
    void prepareSessionExit();
signals:
    void preferencesChanged();
    void loginStartupChanged();
    void trayRegistrationChanged();
    void browserEnableRequested(bool enabled);
    void openFileRequested(const QString& path);
    void showRequested();
    void closingToTray();
    void notice(const QString& message, bool isError);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    bool savePreferences();
    void initializeTray();
    void checkTrayRegistration();
    void queueMinimizeToTray();
    bool hideForMinimize();
};
} // namespace veyra::ui
