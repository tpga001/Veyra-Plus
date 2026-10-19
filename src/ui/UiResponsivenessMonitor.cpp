#include "veyra/ui/UiResponsivenessMonitor.h"
#include "veyra/NativePaintDiagnostics.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QWindow>
#include <QPointer>
#include <QTimer>
#include <QEvent>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
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
qint64 steadyMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct Checkpoint {
    int activeMaximum;
    explicit Checkpoint(int maximum):activeMaximum(maximum){}
    std::atomic<int> stage{-1}, slowStage{-1};
    std::atomic<qint64> changedAt{0}, slowMs{0};
    std::atomic<quint64> calls{0}, slowSequence{0};
    void mark(int next) noexcept {
        if(next<0||next>7)return;
        const auto at=steadyMs();const auto previous=stage.exchange(next);
        ++calls;
        if(previous==next)return;
        const auto began=changedAt.exchange(at);
        // Waiting between ordinary event-loop turns / rendered frames is idle,
        // not evidence of a slow operation. Only bounded active stages count.
        if(previous>=1&&previous<=activeMaximum&&began>0&&at-began>1000){
            slowStage=previous;slowMs=at-began;++slowSequence;
        }
    }
};
Checkpoint uiCheckpoint{5},renderCheckpoint{4};
QString decimal(quint64 v) { return QString::number(v); }
QJsonObject nativePaintSnapshot(const diagnostics::NativePaintCounter& counter) {
    const auto state = counter.snapshot();
    const auto at = steadyMs();
    return {{"enters", decimal(state.enters)}, {"exits", decimal(state.exits)},
        {"depth", double(state.depth)}, {"threadId", double(state.tid)},
        {"windowHwnd", QStringLiteral("0x%1").arg(qulonglong(state.hwnd), 0, 16)},
        {"activeMs", state.depth && state.startMs ? double(std::max<qint64>(0, at-qint64(state.startMs))) : 0.0}};
}

QString address(quintptr v) { return QStringLiteral("0x%1").arg(qulonglong(v), 0, 16); }
bool fixedAction(const QString& s) {
    for (const auto* allowed : {"show", "hide", "close", "minimize", "nav.browser", "nav.pro", "nav.node", "dialog.close", "quit"})
        if (s == QLatin1String(allowed)) return true;
    return false;
}
bool safeAncestors(const QString& path) {
    QString current = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (;;) {
        const QFileInfo info(current);
        if (info.isSymbolicLink()) return false;
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(current).toStdWString();
        const DWORD attr = GetFileAttributesW(native.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        const auto parent = QDir::cleanPath(info.absolutePath());
        if (parent == current) return true;
        current = parent;
    }
}
bool save(const QString& path, const QByteArray& bytes) {
    if (path.isEmpty() || bytes.size() > 65536 || !safeAncestors(path)) return false;
    const auto directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory) || !safeAncestors(path)) return false;
    const QFileInfo existing(path);
    if (existing.exists() && !existing.isFile()) return false;
    QSaveFile file(path); file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
quintptr nativeHandle(QWindow* window) {
    return window && window->handle() ? quintptr(window->winId()) : 0;
}
}

struct UiResponsivenessMonitor::Impl {
    struct Request { QString action; qint64 due; int afterSeconds = 0; };
    Options options;
    QString path;
    quint32 pid = 0; quint64 created = 0;
    QPointer<QWindow> window;
    QTimer timer;
    std::atomic<qint64> lastHeartbeat{0}, lastRender{0}, maxHeartbeatGap{0};
    std::atomic<int> renderStage{-1};
    std::atomic<quint64> heartbeatCount{0}, renderCount{0};
    std::array<std::atomic<quint64>,4> mouse{};
    std::atomic<int> lastMouseType{0};
    std::mutex mutex;
    std::condition_variable wake, completed;
    QJsonObject context;
    std::deque<Request> requests;
    std::deque<QJsonObject> events;
    bool stateKnown = false, stalled = false, renderStalled = false, exit = false;
    qint64 lastInputWrite = -5000;
    quint64 previousMouse = 0, sequence = 0, testRequested = 0, testCompleted = 0;
    quint64 seenUiSlow = 0, seenRenderSlow = 0;
    std::jthread worker;
    qint64 now() const { return options.monotonicMilliseconds ? options.monotonicMilliseconds() : steadyMs(); }
    QJsonObject sample(qint64 at) const {
        QJsonObject out = context;
        out["heartbeatAgeMs"] = double(std::max<qint64>(0, at-lastHeartbeat.load()));
        out["nativeHeartbeatAgeMs"] = out.value("heartbeatAgeMs");
        out["nativePaint"] = QJsonObject{{"overlay", nativePaintSnapshot(diagnostics::overlayPaint)},
            {"syncWindowUpdate", nativePaintSnapshot(diagnostics::syncWindowUpdate)}};
        out["heartbeatCount"] = decimal(heartbeatCount.load());
        out["maxHeartbeatGapMs"] = double(maxHeartbeatGap.load());
        out["uiStage"] = uiCheckpoint.stage.load();
        out["uiStageAgeMs"] = uiCheckpoint.changedAt.load()?double(std::max<qint64>(0,steadyMs()-uiCheckpoint.changedAt.load())):-1.0;
        out["lastSlowUiStage"] = uiCheckpoint.slowStage.load();
        out["lastSlowUiStageMs"] = double(uiCheckpoint.slowMs.load());
        out["renderStage"] = renderCheckpoint.stage.load();
        out["renderCheckpointCount"] = decimal(renderCheckpoint.calls.load());
        out["uiCheckpointCount"] = decimal(uiCheckpoint.calls.load());
        out["renderStageAgeMs"] = renderCheckpoint.changedAt.load()?double(std::max<qint64>(0,steadyMs()-renderCheckpoint.changedAt.load())):-1.0;
        out["lastSlowRenderStage"] = renderCheckpoint.slowStage.load();
        out["lastSlowRenderStageMs"] = double(renderCheckpoint.slowMs.load());
        out["renderCount"] = decimal(renderCount.load());
        out["renderAgeMs"] = lastRender.load() ? double(std::max<qint64>(0, at-lastRender.load())) : -1.0;
        const char* keys[] = {"mousePress", "mouseRelease", "nonClientPress", "nonClientRelease"};
        for (size_t i=0;i<mouse.size();++i) out[keys[i]] = decimal(mouse[i].load());
        out["lastMouseEventType"] = lastMouseType.load();
        return out;
    }
    void event(const QString& kind, qint64 at, int after = -1) {
        QJsonObject item{{"kind",kind},{"monotonicMs",double(at)},{"capturedUtcMs",double(QDateTime::currentMSecsSinceEpoch())},{"snapshot",sample(at)}};
        if (after >= 0) item["afterSeconds"] = after;
        events.push_back(item);
        while (events.size()>16) events.pop_front();
    }
    void run(std::stop_token stop) {
        const bool periodicProbe = qEnvironmentVariable("VEYRA_TEST_UI_PERIODIC_SNAPSHOT") == QLatin1String("1");
        qint64 lastProbeWrite = -1000;
        while (!stop.stop_requested()) {
            QByteArray payload;
            quint64 completing = 0;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock,std::chrono::milliseconds(250));
                if (exit || stop.stop_requested()) break;
                const auto at=now(); bool changed=false;
                if (periodicProbe && at-lastProbeWrite>=1000) {
                    lastProbeWrite=at;event(QStringLiteral("probe.sample"),at);changed=true;
                }
                const bool late=heartbeatCount.load()>0 && at-lastHeartbeat.load()>2000;
                const auto uiSlow=uiCheckpoint.slowSequence.load(),renderSlow=renderCheckpoint.slowSequence.load();
                if(uiSlow!=seenUiSlow){seenUiSlow=uiSlow;event(QStringLiteral("ui.slow-stage"),at);changed=true;}
                if(renderSlow!=seenRenderSlow){seenRenderSlow=renderSlow;event(QStringLiteral("render.slow-stage"),at);changed=true;}
                if (late&&!stalled) { stalled=true; event(QStringLiteral("gui.stall"),at);changed=true; }
                else if (!late&&stalled) { stalled=false;event(QStringLiteral("gui.recovered"),at);changed=true; }
                const auto render=renderCheckpoint.stage.load();
                const auto renderBegan=renderCheckpoint.changedAt.load();
                const bool renderLate=render>=1&&render<=4&&renderBegan>0&&steadyMs()-renderBegan>2000;
                if(renderLate&&!renderStalled){renderStalled=true;event(QStringLiteral("render.stall"),at);changed=true;}
                else if(!renderLate&&renderStalled){renderStalled=false;event(QStringLiteral("render.recovered"),at);changed=true;}
                for(auto it=requests.begin();it!=requests.end();) {
                    if(it->due>at){++it;continue;}
                    event(it->action,at,it->afterSeconds);it=requests.erase(it);changed=true;
                }
                const auto input=mouse[0].load()+mouse[1].load()+mouse[2].load()+mouse[3].load();
                if (input!=previousMouse && at-lastInputWrite>=5000) {
                    previousMouse=input;lastInputWrite=at;event(QStringLiteral("input.counts"),at);changed=true;
                }
                if(changed) {
                    QJsonArray list;for(const auto& e:events)list.append(e);
                    payload=QJsonDocument(QJsonObject{{"schema",1},{"pid",double(pid)},{"processCreatedFileTime",decimal(created)},
                        {"sequence",decimal(++sequence)},{"stalled",stalled},{"renderStalled",renderStalled},{"latest",sample(at)},{"events",list}}).toJson(QJsonDocument::Compact);
                }
                completing=testRequested;
            }
            // No Logger/file-sink lock. All directory/atomic file I/O is here.
            if(!payload.isEmpty())(void)save(path,payload);
            {std::lock_guard lock(mutex);testCompleted=std::max(testCompleted,completing);}
            completed.notify_all();
        }
    }
};

UiResponsivenessMonitor::UiResponsivenessMonitor(QString root,QObject* parent)
    : UiResponsivenessMonitor(std::move(root),Options{},parent) {}
UiResponsivenessMonitor::UiResponsivenessMonitor(QString root,Options options,QObject* parent)
    : QObject(parent),p_(std::make_unique<Impl>()) {
    p_->options=std::move(options);
#ifdef Q_OS_WIN
    FILETIME c{},e{},k{},u{};
    if(QDir::isAbsolutePath(root)&&GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u)) {
        p_->pid=GetCurrentProcessId();p_->created=quint64(c.dwHighDateTime)<<32|c.dwLowDateTime;
        p_->path=QDir::cleanPath(root)+QStringLiteral("/diagnostics/ui-responsiveness-%1-%2.json").arg(p_->pid).arg(p_->created);
    }
#endif
    // Creating the monitor never touches disk or creates a native window.
    p_->lastHeartbeat=p_->now();p_->heartbeatCount=1;
    connect(&p_->timer,&QTimer::timeout,this,&UiResponsivenessMonitor::heartbeat);
    if(p_->options.automaticHeartbeat)p_->timer.start(500);
    if(QCoreApplication::instance())QCoreApplication::instance()->installEventFilter(this);
    p_->worker=std::jthread([impl=p_.get()](std::stop_token stop){impl->run(stop);});
}
UiResponsivenessMonitor::~UiResponsivenessMonitor() {
    if(QCoreApplication::instance())QCoreApplication::instance()->removeEventFilter(this);
    p_->timer.stop();
    {std::lock_guard lock(p_->mutex);p_->exit=true;}
    p_->worker.request_stop();p_->wake.notify_all();p_->completed.notify_all();
    if(p_->worker.joinable())p_->worker.join();
    // Keep this process's final diagnostic; never clean another process's file.
}
void UiResponsivenessMonitor::attachWindow(QWindow* window) {p_->window=window;heartbeat();}
QString UiResponsivenessMonitor::diagnosticPath() const{return p_->path;}
void UiResponsivenessMonitor::heartbeatForTest(){heartbeat();}
bool UiResponsivenessMonitor::pollForTest(unsigned timeout) {
    std::unique_lock lock(p_->mutex);const auto ticket=++p_->testRequested;p_->wake.notify_one();
    return p_->completed.wait_for(lock,std::chrono::milliseconds(timeout),[&]{return p_->testCompleted>=ticket||p_->exit;});
}
void UiResponsivenessMonitor::heartbeat() {
    const auto at=p_->now();QJsonObject state{{"windowAttached",bool(p_->window)},{"sampledMonotonicMs",double(at)}};
    const auto handle=nativeHandle(p_->window);
    if(p_->window) {
        state["visible"]=p_->window->isVisible();state["active"]=p_->window->isActive();
        state["visibility"]=int(p_->window->visibility());state["modality"]=int(p_->window->modality());
    }
    state["windowHwnd"]=address(handle);
    if(qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        state["focusHwnd"]=address(nativeHandle(QGuiApplication::focusWindow()));
        state["modalHwnd"]=address(nativeHandle(QGuiApplication::modalWindow()));
    }
#ifdef Q_OS_WIN
    const auto native=reinterpret_cast<HWND>(handle);
    state["enabled"]=handle?bool(IsWindowEnabled(native)):true;
    state["ownerHwnd"]=address(quintptr(handle?GetWindow(native,GW_OWNER):nullptr));
    const auto front=GetForegroundWindow();DWORD frontPid=0;if(front)GetWindowThreadProcessId(front,&frontPid);
    state["foregroundHwnd"]=address(quintptr(front));state["foregroundPid"]=double(frontPid);
    GUITHREADINFO gui{sizeof(gui)};
    if(GetGUIThreadInfo(GetCurrentThreadId(),&gui)) {
        state["guiFlags"]=double(gui.flags);state["captureHwnd"]=address(quintptr(gui.hwndCapture));
        state["menuOwnerHwnd"]=address(quintptr(gui.hwndMenuOwner));state["moveSizeHwnd"]=address(quintptr(gui.hwndMoveSize));
    }
#endif
    const auto gap=std::max<qint64>(0,at-p_->lastHeartbeat.exchange(at));
    auto largest=p_->maxHeartbeatGap.load();
    while(gap>largest&&!p_->maxHeartbeatGap.compare_exchange_weak(largest,gap)){}
    ++p_->heartbeatCount;
    {
        std::lock_guard lock(p_->mutex);
        // Enabled/modal/capture changes explain living UI with blocked input.
        bool changed=false;
        if(p_->stateKnown)for(const auto* key:{"enabled","modalHwnd","captureHwnd","menuOwnerHwnd"})
            if(state.value(key)!=p_->context.value(key)){changed=true;break;}
        p_->context=state;p_->stateKnown=true;
        if(changed) {
            p_->requests.push_back({QStringLiteral("window.state"),at,0});
            while(p_->requests.size()>48)p_->requests.pop_front();
        }
    }
    p_->wake.notify_one();
}
void UiResponsivenessMonitor::noteAction(const QString& stage) {
    if(!fixedAction(stage))return;
    heartbeat();const auto at=p_->now();
    {std::lock_guard lock(p_->mutex);
        for(const int delay:{0,5,15})p_->requests.push_back({stage,at+delay*1000,delay});
        while(p_->requests.size()>48)p_->requests.pop_front();
    }
    p_->wake.notify_one();
}
void UiResponsivenessMonitor::noteRenderStage(int stage) noexcept {
    if(stage<0||stage>7)return;
    setRenderStage(stage);
    try {p_->lastRender=p_->now();++p_->renderCount;}catch(...){}
}
void UiResponsivenessMonitor::setUiStage(int stage) noexcept { uiCheckpoint.mark(stage); }
void UiResponsivenessMonitor::setRenderStage(int stage) noexcept { renderCheckpoint.mark(stage); }
bool UiResponsivenessMonitor::eventFilter(QObject* watched,QEvent* event) {
    int index=-1;
    switch(event->type()) {
    case QEvent::MouseButtonPress:index=0;break;
    case QEvent::MouseButtonRelease:index=1;break;
    case QEvent::NonClientAreaMouseButtonPress:index=2;break;
    case QEvent::NonClientAreaMouseButtonRelease:index=3;break;
    default:return false;
    }
    // Application events target this process. Count only a QWindow or objects
    // descending from one; no global hooks and no key/value/position recording.
    bool own=false;int depth=0;
    for(auto* item=watched;item&&depth++<64;item=item->parent())if(qobject_cast<QWindow*>(item)){own=true;break;}
    if(own){++p_->mouse[size_t(index)];p_->lastMouseType=int(event->type());}
    return false;
}
} // namespace veyra::ui
