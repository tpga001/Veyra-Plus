#include "veyra/ui/UiResponsivenessMonitor.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <atomic>
#include <iostream>
#include <memory>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using veyra::ui::UiResponsivenessMonitor;
namespace {
int checks=0,failures=0;
bool check(bool value,const char* label){++checks;if(!value){++failures;std::cerr<<"FAIL "<<label<<'\n';}return value;}
QByteArray read(const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
QJsonObject object(const QString& path){return QJsonDocument::fromJson(read(path)).object();}
int eventsOf(const QJsonObject& obj,const char* kind){int n=0;for(const auto& v:obj.value("events").toArray())if(v.toObject().value("kind").toString()==QLatin1String(kind))++n;return n;}
bool write(const QString& path,const QByteArray& data){QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(data)==data.size();}
bool junction(const QString& link,const QString& target){
#ifdef Q_OS_WIN
 QProcess p;p.start(QStringLiteral("cmd.exe"),{QStringLiteral("/d"),QStringLiteral("/c"),QStringLiteral("mklink"),QStringLiteral("/J"),QDir::toNativeSeparators(link),QDir::toNativeSeparators(target)});
 return p.waitForFinished(3000)&&p.exitStatus()==QProcess::NormalExit&&p.exitCode()==0;
#else
 Q_UNUSED(link);Q_UNUSED(target);return false;
#endif
}
}
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);
 if(app.arguments().size()!=2){std::cerr<<"usage: <new absolute E-drive scratch directory>\n";return 2;}
 const auto root=QDir::cleanPath(app.arguments()[1]);
 if(!QDir::isAbsolutePath(root)||!root.startsWith("E:/",Qt::CaseInsensitive)||QFileInfo::exists(root)||!QDir().mkpath(root))return 2;
 std::atomic<qint64> clock{1000};
 UiResponsivenessMonitor::Options opts;opts.automaticHeartbeat=false;opts.monotonicMilliseconds=[&]{return clock.load();};
 const auto own=root+"/normal";
 auto monitor=std::make_unique<UiResponsivenessMonitor>(own,opts);const auto path=monitor->diagnosticPath();
 check(!path.isEmpty()&&!QFileInfo::exists(own),"constructor performs no disk write");
 check(monitor->pollForTest(),"background watcher synchronizes");
 check(!QFileInfo::exists(own),"healthy idle does not write continuously");
 monitor->noteAction(QStringLiteral("F:/private-title/video.mp4"));check(monitor->pollForTest(),"invalid action poll");
 check(!QFileInfo::exists(own),"untrusted action text is rejected");
 UiResponsivenessMonitor::setUiStage(1);UiResponsivenessMonitor::setUiStage(0);
 monitor->noteAction(QStringLiteral("show"));check(monitor->pollForTest(),"initial action publishes asynchronously");
 auto packet=object(path);check(!packet.isEmpty()&&packet.value("schema").toInt()==1,"atomic file contains valid JSON");
 check(packet.value("latest").toObject().value("uiStage").toInt()==0,"static UI checkpoint included");
 check(eventsOf(packet,"show")==1&&!packet.value("stalled").toBool(),"show captures zero-second healthy snapshot");
 for(int i=0;i<4;++i){clock+=500;monitor->heartbeatForTest();check(monitor->pollForTest(),"normal heartbeat poll");}
 check(eventsOf(object(path),"gui.stall")==0,"normal heartbeat never reports stall");
 clock+=2501;check(monitor->pollForTest(),"fake-clock stall poll without blocking GUI");
 packet=object(path);check(packet.value("stalled").toBool()&&eventsOf(packet,"gui.stall")==1,"independent watcher records a missing heartbeat");
 clock+=500;check(monitor->pollForTest(),"continued stall poll");
 check(eventsOf(object(path),"gui.stall")==1,"persistent stall does not flood logs");
 monitor->heartbeatForTest();check(monitor->pollForTest(),"recovery poll");
 packet=object(path);check(!packet.value("stalled").toBool()&&eventsOf(packet,"gui.recovered")==1,"recovery is preserved after UI returns");
 check(packet.value("latest").toObject().value("maxHeartbeatGapMs").toDouble()>=3001,"longest heartbeat gap retained");
 clock=16000;monitor->heartbeatForTest();monitor->noteRenderStage(3);check(monitor->pollForTest(),"scheduled snapshots poll");
 packet=object(path);check(eventsOf(packet,"show")==3,"show has bounded zero five and fifteen second snapshots");
 check(packet.value("latest").toObject().value("renderStage").toInt()==3,"numeric render stage is included without GPU work");
 UiResponsivenessMonitor::setRenderStage(5);
 for(int i=0;i<25;++i){clock+=100;monitor->noteAction(QStringLiteral("nav.browser"));check(monitor->pollForTest(),"bounded action poll");}
 packet=object(path);check(packet.value("events").toArray().size()<=16&&read(path).size()<=65536,"event tail and file size are bounded");
 const auto bytes=read(path);
 check(!bytes.contains("private-title")&&!bytes.contains("mediaPath")&&!bytes.contains("windowTitle")&&!bytes.contains("coordinates")&&!bytes.contains("keyValue"),"diagnostic never records media text pointer coordinates or key values");
 const auto other=root+"/other-process.json";check(write(other,"foreign sentinel"),"create unrelated sentinel");
 monitor.reset();check(QFileInfo::exists(path)&&read(other)=="foreign sentinel","exit preserves diagnostics and unrelated files");
 const auto target=root+"/junction-target",link=root+"/junction-link";
 check(QDir().mkpath(target)&&junction(link,target),"create private reparse-ancestor fixture");
 {UiResponsivenessMonitor blocked(link+"/nested",opts);blocked.noteAction(QStringLiteral("show"));check(blocked.pollForTest(),"reparse rejection poll");}
 check(!QFileInfo::exists(target+"/nested"),"reparse ancestors rejected before writes");
#ifdef Q_OS_WIN
 check(RemoveDirectoryW(QDir::toNativeSeparators(link).toStdWString().c_str())!=FALSE,"remove only the test junction itself");
#endif
 std::cout<<"checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
