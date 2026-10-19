#include "veyra/ui/DisplayMonitor.h"
#include "veyra/Log.h"
#include <QRegularExpression>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVariantList>
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <limits>
namespace veyra::ui {
namespace {
double qpcMs(){LARGE_INTEGER n{},f{};QueryPerformanceCounter(&n);QueryPerformanceFrequency(&f);return double(n.QuadPart)*1000.0/double(f.QuadPart);}
uint64_t processCreated(HANDLE p){FILETIME c{},e{},k{},u{};if(!GetProcessTimes(p,&c,&e,&k,&u))return 0;return (uint64_t(c.dwHighDateTime)<<32)|c.dwLowDateTime;}
QString statusText(const QString& state){
    if(state=="live")return QStringLiteral("\u5b9e\u65f6\u663e\u793a\u4e8b\u4ef6 \u00b7 \u6700\u8fd1 5 \u79d2");
    if(state=="frozen")return QStringLiteral("\u5df2\u51bb\u7ed3 \u00b7 \u4fdd\u7559\u6700\u540e\u663e\u793a\u8bfb\u6570");
    if(state=="permission")return QStringLiteral("\u9700\u8981\u663e\u793a\u91c7\u96c6\u6743\u9650");
    if(state=="starting")return QStringLiteral("\u6b63\u5728\u542f\u52a8\u663e\u793a\u76d1\u6d4b");
    if(state=="warming")return QStringLiteral("\u6b63\u5728\u91c7\u6837 \u00b7 \u7b49\u5f85\u6709\u6548\u663e\u793a\u4e8b\u4ef6");
    if(state=="unmatched")return QStringLiteral("\u89c6\u9891\u8f93\u51fa\u6d41\u5c1a\u672a\u5339\u914d");
    if(state=="stale")return QStringLiteral("\u663e\u793a\u4e8b\u4ef6\u5df2\u4e2d\u65ad");
    if(state=="unavailable")return QStringLiteral("\u663e\u793a\u91c7\u96c6\u7ec4\u4ef6\u4e0d\u53ef\u7528");
    if(state=="error")return QStringLiteral("\u663e\u793a\u76d1\u6d4b\u5f02\u5e38");
    return QStringLiteral("\u7b49\u5f85\u64ad\u653e");
}
QString imagePath(DWORD pid){HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!h)return {};wchar_t path[32768]{};DWORD n=32768;const bool ok=QueryFullProcessImageNameW(h,0,path,&n);CloseHandle(h);return ok?QDir::cleanPath(QString::fromWCharArray(path,n)):QString{};}
}
struct DisplayMonitor::Impl {
    DisplayMonitor* owner;
    QTimer tick;
    QLocalServer server;
    QProcess normal;
    QPointer<QLocalSocket> socket;
    QByteArray incoming;
    diagnostics::DisplayCsvParser parser;
    diagnostics::DisplayMetricsWindow window,discovery;
    DisplayMonitorSource source;
    std::function<DisplayMonitorSource()> provider;
    QVariantMap value,frozen;
    std::vector<double> frozenDurations;
    QString phase="off",detail,nonce,pipe;
    bool attempted=false,permissionRequired=false,connecting=false,ready=false,stopping=false,blocked=false;
    bool wasPlaying=false,generatedSeen=false,handshaken=false;
    double boundary=0,started=0,generatedAt=0;
    DWORD helperPid=0;
    HANDLE elevatedProcess=nullptr;
    uint64_t received=0,matched=0,parseErrors=0;
    uint64_t mappedAddress=0;
    explicit Impl(DisplayMonitor* q):owner(q){
        tick.setInterval(250);
        QObject::connect(&tick,&QTimer::timeout,q,[this]{update();});
        QObject::connect(&server,&QLocalServer::newConnection,q,[this]{accept();});
        QObject::connect(&normal,&QProcess::finished,q,[this](int exit,QProcess::ExitStatus){
            if(stopping)return;
            ready=false;connecting=false;
            if(exit==6){permissionRequired=true;phase="permission";}
            else if(phase!="permission"&&phase!="error"&&phase!="unavailable"){phase="error";detail=QStringLiteral("collector-exited-%1").arg(exit);}
            window.markUnavailable(detail.toStdString());publish();
        });
        QObject::connect(&normal,&QProcess::errorOccurred,q,[this](QProcess::ProcessError error){
            if(!stopping&&error==QProcess::FailedToStart){ready=connecting=false;phase="error";detail="collector-start-failed";window.markUnavailable(detail.toStdString());publish();}
        });
        normal.setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(&normal,&QProcess::readyReadStandardOutput,q,[this]{normal.readAllStandardOutput();});
        tick.start();publish();
    }
    ~Impl(){
        stopping=true;tick.stop();stop();
        // Shutdown only: let the helper finish its own bounded ETW cleanup.
        if(normal.state()!=QProcess::NotRunning)normal.waitForFinished(4500);
        if(elevatedProcess){WaitForSingleObject(elevatedProcess,4500);CloseHandle(elevatedProcess);}
    }
    QString helperPath()const{return QDir(QCoreApplication::applicationDirPath()).filePath("veyra_display_collector.exe");}
    void send(const QString& type){if(socket){QJsonObject v{{"schema",1},{"nonce",nonce},{"type",type}};socket->write(QJsonDocument(v).toJson(QJsonDocument::Compact)+'\n');socket->flush();}}
    void stop(){
        send("stop");
        if(socket){socket->disconnectFromServer();socket=nullptr;}
        server.close();
        // Pipe disconnect asks the helper to clean only its own trace asynchronously.
        ready=connecting=false;
    }
    void resetSource(const DisplayMonitorSource& next,double now){
        source=next;boundary=now;mappedAddress=0;generatedSeen=false;generatedAt=0;matched=0;frozen.clear();frozenDurations.clear();wasPlaying=false;
        window.reset(next.target,now);window.setExpectedGeneratedFrameType(next.expectedGeneratedFrameType);
        auto unbound=next.target;unbound.outputSwapChain=0;discovery.reset(unbound,now);discovery.setExpectedGeneratedFrameType(next.expectedGeneratedFrameType);
    }
    void update(){
        const auto now=qpcMs();
        const auto next=provider?provider():DisplayMonitorSource{};
        if(next.target!=source.target||next.sourceKind!=source.sourceKind||next.available!=source.available){resetSource(next,now);}
        else {source.playing=next.playing;source.expectedGeneratedFrameType=next.expectedGeneratedFrameType;}
        if(source.playing&&!wasPlaying){auto resumed=source;resetSource(resumed,now);}
        if(!source.playing&&wasPlaying&&value.value("known").toBool()){frozenDurations=window.displayedDurations(now);frozen=value;frozen["state"]="frozen";frozen["status"]=statusText("frozen");frozen["frozen"]=true;}
        wasPlaying=source.playing;
        if(source.available&&source.playing&&!attempted&&!connecting)launch(false);
        if(connecting&&now-started>90000){phase="permission";permissionRequired=true;detail="collector-start-timeout";connecting=false;}
        if(elevatedProcess&&WaitForSingleObject(elevatedProcess,0)==WAIT_OBJECT_0){DWORD code=1;GetExitCodeProcess(elevatedProcess,&code);CloseHandle(elevatedProcess);elevatedProcess=nullptr;ready=connecting=false;if(!stopping){phase=code==6?"permission":"error";permissionRequired=code==6;detail=QStringLiteral("collector-exited-%1").arg(code);window.markUnavailable(detail.toStdString());}}
        publish();
    }
    void awaitStopped(bool elevate,double deadline){
        QTimer::singleShot(100,owner,[this,elevate,deadline]{
            const bool active=normal.state()!=QProcess::NotRunning||(elevatedProcess&&WaitForSingleObject(elevatedProcess,0)==WAIT_TIMEOUT);
            if(active&&qpcMs()<deadline){awaitStopped(elevate,deadline);return;}
            stopping=false;connecting=false;
            if(active){phase="error";detail="collector-cleanup-timeout";window.markUnavailable(detail.toStdString());publish();return;}
            launch(elevate);
        });
    }
    void launch(bool elevated){
        if(connecting||ready)return;
        attempted=true;
        if(normal.state()!=QProcess::NotRunning||(elevatedProcess&&WaitForSingleObject(elevatedProcess,0)==WAIT_TIMEOUT)){
            stopping=true;stop();connecting=true;phase="starting";started=qpcMs();
            awaitStopped(elevated,started+5000);return;
        }
        blocked=false;handshaken=false;parseErrors=received=matched=0;parser={};
        const auto exe=helperPath();const auto tool=QDir(QCoreApplication::applicationDirPath()).filePath("tools/PresentMon.exe");
        if(!QFileInfo::exists(exe)||!QFileInfo::exists(tool)){phase="unavailable";detail="collector-files-missing";publish();return;}
        stopping=true;stop();stopping=false;
        if(elevatedProcess){CloseHandle(elevatedProcess);elevatedProcess=nullptr;}
        nonce=QUuid::createUuid().toString(QUuid::Id128)+QUuid::createUuid().toString(QUuid::Id128);
        pipe=QStringLiteral("VeyraDisplay-%1-%2").arg(GetCurrentProcessId()).arg(nonce.left(32));
        server.setSocketOptions(QLocalServer::UserAccessOption);
        if(!server.listen(pipe)){phase="error";detail="private-pipe-create-failed";publish();return;}
        incoming.clear();helperPid=0;connecting=true;ready=false;phase="starting";detail.clear();started=qpcMs();
        const QStringList args{"--pipe",pipe,"--nonce",nonce,"--parent-pid",QString::number(GetCurrentProcessId()),"--parent-created",QString::number(processCreated(GetCurrentProcess() ))};
        if(!elevated){
            normal.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a){a->flags|=CREATE_NO_WINDOW;});
            normal.start(exe,args);
            return;
        }
        // UAC can wait for a person. Keep it off the UI/renderer thread.
        const QString parameters=args.join(' ');const auto token=nonce;QPointer<DisplayMonitor> guard(owner);
        auto* thread=QThread::create([exe,parameters,guard,token]{
            const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            SHELLEXECUTEINFOW e{};e.cbSize=sizeof(e);e.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;e.lpVerb=L"runas";const auto file=exe.toStdWString(),args=parameters.toStdWString();e.lpFile=file.c_str();e.lpParameters=args.c_str();e.nShow=SW_HIDE;
            const bool ok=ShellExecuteExW(&e);const DWORD error=ok?0:GetLastError();HANDLE process=ok?e.hProcess:nullptr;
            if(SUCCEEDED(com))CoUninitialize();
            if(!guard){if(process)CloseHandle(process);return;}
            QMetaObject::invokeMethod(guard,[guard,token,process,error]{
                if(!guard){if(process)CloseHandle(process);return;}auto& p=*guard->p_;
                if(p.nonce!=token||p.stopping){if(process)CloseHandle(process);return;}
                if(error){p.connecting=false;p.ready=false;p.phase="permission";p.permissionRequired=true;p.detail=QStringLiteral("authorization-%1").arg(error);p.publish();}
                else {p.elevatedProcess=process;const DWORD pid=GetProcessId(process);if(p.helperPid&&pid!=p.helperPid){p.phase="error";p.detail="collector-pid-mismatch";p.send("stop");p.ready=false;}p.helperPid=pid;}
            },Qt::QueuedConnection);
        });
        QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);thread->start();
    }
    void accept(){
        while(server.hasPendingConnections()){
            auto* s=server.nextPendingConnection();if(socket||!connecting){s->disconnectFromServer();s->deleteLater();continue;}
            ULONG pid=0;const auto handle=reinterpret_cast<HANDLE>(s->socketDescriptor());
            const bool peer=GetNamedPipeClientProcessId(handle,&pid)&&pid&&imagePath(pid).compare(QDir::cleanPath(helperPath()),Qt::CaseInsensitive)==0;
            if(!peer||(helperPid&&helperPid!=pid)){s->disconnectFromServer();s->deleteLater();continue;}
            helperPid=pid;socket=s;
            QObject::connect(s,&QLocalSocket::readyRead,owner,[this,s]{read(s);});
            QObject::connect(s,&QLocalSocket::disconnected,owner,[this,s]{if(socket==s){socket=nullptr;ready=false;connecting=false;if(!stopping&&phase!="permission"&&phase!="error"){phase="error";detail="collector-disconnected";window.markUnavailable(detail.toStdString());}publish();}s->deleteLater();});
            if(s->bytesAvailable())read(s);
        }
    }
    void reject(const QString& why){blocked=true;ready=false;phase="error";detail=why;window.markUnavailable(why.toStdString());send("stop");publish();}
    void read(QLocalSocket* s){
        if(socket!=s||stopping)return;
        incoming+=s->readAll();if(incoming.size()>512*1024){reject("collector-input-overflow");incoming.clear();return;}
        for(qsizetype at;(at=incoming.indexOf('\n'))>=0;){
            const auto line=incoming.left(at);incoming.remove(0,at+1);if(line.size()>256*1024){reject("collector-line-overflow");continue;}
            QJsonParseError err;const auto doc=QJsonDocument::fromJson(line,&err);const auto o=doc.object();
            if(err.error!=QJsonParseError::NoError||o.value("schema").toInt()!=1||o.value("nonce").toString()!=nonce){reject("collector-protocol-invalid");continue;}
            const auto type=o.value("type").toString();
            if(type=="ready"&&!blocked){
                if(handshaken){reject("collector-duplicate-ready");continue;}
                if(o.value("helperPid").toInt()!=int(helperPid)){reject("collector-identity-invalid");continue;}
                handshaken=true;ready=true;connecting=false;phase="warming";permissionRequired=false;auto current=source;resetSource(current,qpcMs());send("start");
            }else if(type=="state"){
                const auto state=o.value("state").toString();
                if(state=="permission"){ready=connecting=false;permissionRequired=true;phase="permission";detail="system-trace-access-denied";window.markUnavailable(detail.toStdString());}
                else if(state=="error"){reject(o.value("message").toString("collector-error").left(160));}
            }else if(type=="diagnostic"){
                const auto message=o.value("message").toString().left(1024);const auto lower=message.toLower();
                if(lower.contains("access denied")){permissionRequired=true;phase="permission";ready=false;window.markUnavailable("system-trace-access-denied");}
                else if((lower.contains("lost")||lower.contains("dropped"))&&(lower.contains("event")||lower.contains("buffer"))){
                    // Only an entire, explicitly zero diagnostic is harmless.
                    // A zero substring must not hide another positive loss count.
                    static const QRegularExpression zeroLoss(QStringLiteral("^\\s*(?:0\\s+(?:events?|buffers?)\\s+(?:were\\s+)?(?:lost|dropped)|(?:lost|dropped)\\s+(?:events?|buffers?)\\s*[:=]\\s*0)[.!\\s]*$"));
                    if(!zeroLoss.match(lower).hasMatch())reject("display-trace-events-lost");
                }
            }else if(type=="csv"&&!blocked){consume(o.value("line").toString());}
        }
    }
    void consume(const QString& csv){
        const auto bytes=csv.toUtf8();std::string error;
        if(!parser.ready()){
            if(!parser.parseHeader(std::string_view(bytes.constData(),size_t(bytes.size())),&error)){reject(QString::fromStdString(error));return;}
            phase="warming";return;
        }
        const auto record=parser.parseRecord(std::string_view(bytes.constData(),size_t(bytes.size())),&error);
        if(!record){++parseErrors;reject("display-csv-invalid");return;}
        ++received;if(!source.available||!source.playing)return;
        const auto now=qpcMs();
        discovery.add(*record);
        const auto admitted=window.add(*record);
        if(admitted==diagnostics::DisplayRecordAdmission::Accepted){++matched;if(record->kind==diagnostics::DisplayRecordKind::Displayed&&record->displayStartQpcMs&&!source.expectedGeneratedFrameType.empty()&&record->frameType==source.expectedGeneratedFrameType){generatedSeen=true;generatedAt=*record->displayStartQpcMs+record->displayedMs;}}
        const auto measured=window.snapshot(now);
        // A provider proxy may differ from the ETW chain. Only a unique stable
        // SDK-generated stream can suggest a replacement, never a Qt FPS rank.
        if(!measured.known()&&!mappedAddress){
            const auto candidate=discovery.mappingCandidate(now);
            if(candidate&&*candidate!=source.target.outputSwapChain){
                auto target=source.target;target.outputSwapChain=*candidate;window.reset(target,now);window.setExpectedGeneratedFrameType(source.expectedGeneratedFrameType);mappedAddress=*candidate;boundary=now;
                log::info("display-monitor","confirmed unique provider-generated output mapping");
            }
        }
    }
    void publish(){
        QVariantMap next;
        QString state=phase;const auto now=qpcMs();const auto m=window.snapshot(now);
        bool valid=false;
        if(!source.available){state="off";}
        else if(!source.playing){
            if(!frozen.isEmpty())next=frozen;
            else state="off";
        }else if(phase=="permission"||phase=="error"||phase=="unavailable"||connecting){state=connecting?"starting":phase;}
        else if(ready&&!blocked){
            valid=m.known();
            if(valid)state="live";
            else if(m.state==diagnostics::DisplayMeasurementState::Unavailable)state="error";
            else if(m.state==diagnostics::DisplayMeasurementState::Stale)state=matched?"stale":"unmatched";
            else if(now-boundary>=5000&&!m.lastDisplayQpcMs)state=matched?"stale":"unmatched";
            else state="warming";
        }
        if(next.isEmpty()){
            next={{"state",state},{"status",statusText(state)},{"known",valid},{"fps",valid?*m.fps:0.0},{"onePercentLowKnown",valid&&m.onePercentLowFps.has_value()},{"onePercentLowFps",valid?m.onePercentLowFps.value_or(0):0},
                {"displayedFrames",QVariant::fromValue<qulonglong>(m.displayedFrames)},{"notDisplayedPresents",QVariant::fromValue<qulonglong>(m.notDisplayedPresents)},{"unresolvedFrames",QVariant::fromValue<qulonglong>(m.unresolvedPresents)},
                {"windowSeconds",5.0},{"sampleSeconds",m.displayedSpanMs/1000.0},{"includesGeneratedFrames",valid&&generatedSeen&&now-generatedAt<5000},{"frozen",false}};
        }
        next["sourceKind"]=QString::fromStdString(source.sourceKind);next["sessionId"]=QVariant::fromValue<qulonglong>(source.target.sessionId);next["epoch"]=QVariant::fromValue<qulonglong>(source.target.epoch);next["revision"]=QVariant::fromValue<qulonglong>(source.target.revision);
        next["outputSwapchain"]=QStringLiteral("0x%1").arg(mappedAddress?mappedAddress:source.target.outputSwapChain,0,16);next["expectedOutputSwapchain"]=QStringLiteral("0x%1").arg(source.target.outputSwapChain,0,16);next["outputMapping"]=mappedAddress?"sdk-generated-stream":"exact-native-address";next["collectorPid"]=quint32(helperPid);next["receivedRecords"]=QVariant::fromValue<qulonglong>(received);next["matchedRecords"]=QVariant::fromValue<qulonglong>(matched);next["errorCode"]=detail;next["measurementSource"]="PresentMon Windows display events";
        if(value!=next){value=next;emit owner->changed();}
    }
};
DisplayMonitor::DisplayMonitor(QObject* parent):QObject(parent),p_(std::make_unique<Impl>(this)){}
DisplayMonitor::~DisplayMonitor()=default;
void DisplayMonitor::setSourceProvider(std::function<DisplayMonitorSource()> provider){p_->provider=std::move(provider);}
QVariantMap DisplayMonitor::metrics()const{return p_->value;}
double DisplayMonitor::fps()const{return p_->value.value("known").toBool()?p_->value.value("fps").toDouble():0;}
bool DisplayMonitor::known()const{return p_->value.value("known").toBool();}
void DisplayMonitor::authorize(){if(!p_->source.available)return;p_->attempted=false;p_->launch(p_->permissionRequired);}
std::vector<double> DisplayMonitor::frameDurations()const{if(!known())return {};return p_->value.value("frozen").toBool()?p_->frozenDurations:p_->window.displayedDurations(qpcMs());}
}
