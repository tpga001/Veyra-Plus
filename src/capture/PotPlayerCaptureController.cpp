#include "veyra/capture/PotPlayerCaptureController.h"
#include "veyra/source/ScreenCaptureSource.h"
#include "veyra/Log.h"
#include <algorithm>
#include <format>

namespace veyra::capture {
namespace {
bool sameRect(const RECT& a,const RECT& b) { return EqualRect(&a,&b)!=FALSE; }
LONG width(const RECT& r) { return r.right-r.left; }
LONG height(const RECT& r) { return r.bottom-r.top; }
bool sameCapture(const PotPlayerVideoGeometry& a,const PotPlayerVideoGeometry& b) {
    if(a.playerWindow!=b.playerWindow||a.videoWindow!=b.videoWindow||
       a.processId!=b.processId||a.dpi!=b.dpi||a.monitor!=b.monitor)return false;
    const auto& x=a.extendedFrameRect;const auto& y=b.extendedFrameRect;
    const auto& v=a.videoRect;const auto& w=b.videoRect;
    return width(x)==width(y)&&height(x)==height(y)&&width(v)==width(w)&&height(v)==height(w)&&
           v.left-x.left==w.left-y.left&&v.top-x.top==w.top-y.top;
}
}
PotPlayerCaptureController::PotPlayerCaptureController() { engine_.setEnhancementPrewarmEnabled(false); }
PotPlayerCaptureController::~PotPlayerCaptureController() { enabled_=false;overlay_.hide();engine_.stop(); }
void PotPlayerCaptureController::stopSession(const wchar_t* reason) {
    overlay_.hide();
    if(active_.playerWindow||opening_)engine_.stop();
    active_={};opening_=false;haveCandidate_=false;
    status_=reason;
}
void PotPlayerCaptureController::setEnabled(bool enabled) {
    enabled_=enabled;
    if(!enabled)stopSession(L"已关闭，原画面继续由 PotPlayer 播放");
    else {haveCandidate_=false;status_=L"等待前台 PotPlayer 视频区域";}
    log::info("potplayer-attachment",std::format("userEnabled={}",enabled));
}
void PotPlayerCaptureController::fail(const wchar_t* reason) {
    // Failure is latched. A timer, source heartbeat or focus change cannot retry.
    enabled_=false;stopSession(reason);
    log::error("potplayer-attachment","session disabled; explicit user enable required");
}
bool PotPlayerCaptureController::start(const PotPlayerVideoGeometry& g) {
    const auto& frame=g.extendedFrameRect;const auto& video=g.videoRect;
    RECT clipped{};
    if(!IntersectRect(&clipped,&frame,&video)||!sameRect(clipped,video)||width(video)<64||height(video)<64) {
        fail(L"视频区域超出捕获边界，已关闭");return false;
    }
    // start is called only after idle(): the prior swapchain has released this HWND.
    overlay_.destroy();
    if(!overlay_.create(g.playerWindow,video)) {
        fail(L"无法安全创建覆盖窗口，已关闭；详见日志");return false;
    }
    source::ScreenCaptureOptions capture;
    capture.kind=source::ScreenTargetKind::Window;
    capture.method=source::ScreenCaptureMethod::Wgc;
    capture.target=reinterpret_cast<uint64_t>(g.playerWindow);
    capture.fps=0;capture.cursor=false;
    capture.left=unsigned(video.left-frame.left);capture.top=unsigned(video.top-frame.top);
    capture.right=unsigned(frame.right-video.right);capture.bottom=unsigned(frame.bottom-video.bottom);
    engine::EnhancementSettings settings;
    settings.nr=false;settings.sr=false;settings.multiplier=1;
    settings.nrMotion=engine::MotionSource::Zero;settings.srMotion=engine::MotionSource::Zero;
    settings.fgMotion=engine::MotionSource::Zero;
    engine::PresentationSettings presentation;
    presentation.enabled=true;
    presentation.display=engine::DisplaySync::Automatic;
    presentation.outputRate=engine::OutputRateMode::FollowDisplay;
    engine_.requestPresentation(presentation);
    engine_.open(overlay_.handle(),capture.uri(),engine::PlayerOptions::from(settings));
    active_=g;opening_=true;openedAt_=GetTickCount64();haveCandidate_=false;
    status_=L"正在捕获视频区域（无增强对照）";
    log::info("potplayer-attachment",std::format(
        "open pid={} player={} video={} output={} region={},{},{}x{} frame={}x{} crop={},{},{},{} dpi={} monitor={} inputClock=compositor noEffects=1",
        g.processId,uint64_t(g.playerWindow),uint64_t(g.videoWindow),uint64_t(overlay_.handle()),
        video.left,video.top,width(video),height(video),width(frame),height(frame),
        capture.left,capture.top,capture.right,capture.bottom,g.dpi,uint64_t(g.monitor)));
    return true;
}
void PotPlayerCaptureController::tick() {
    if(!enabled_)return;
    const auto now=GetTickCount64();
    const auto probe=probeForegroundPotPlayerVideoWindow();
    if(!probe.ready()) {
        if(active_.playerWindow||opening_)stopSession(L"等待 PotPlayer 回到前台；原画面已恢复");
        haveCandidate_=false;
        if(now>=nextLog_) {
            log::info("potplayer-attachment",std::format("waiting reason={} error={}",potPlayerProbeStatusName(probe.status),probe.win32Error));
            nextLog_=now+2000;
        }
        return;
    }
    const auto& g=probe.geometry;
    if(active_.playerWindow&&sameCapture(active_,g)) {
        if(!overlay_.updateGeometry(g.playerWindow,g.videoRect)) {
            fail(L"窗口跟随失败，已恢复原画面");return;
        }
        active_=g;
        const auto state=engine_.snapshot();
        if(state.failed) { fail(L"捕获或呈现失败，已恢复原画面；详见日志");return; }
        if(state.sourceWidth&&
           (state.sourceWidth!=unsigned(width(g.videoRect))||state.sourceHeight!=unsigned(height(g.videoRect)))) {
            log::error("potplayer-attachment",std::format("capture crop mismatch expected={}x{} actual={}x{}",width(g.videoRect),height(g.videoRect),state.sourceWidth,state.sourceHeight));
            if(++cropMismatches_>=3)fail(L"捕获尺寸连续不匹配，已关闭；详见日志");
            else stopSession(L"捕获尺寸变化，等待重新匹配；原画面已恢复");
            return;
        }
        if(state.running&&state.metrics.flow.counters.realPresented>0) {
            opening_=false;
            if(!overlay_.showAfterPresent()) {
                if(overlay_.lastError()==ERROR_NOT_READY)stopSession(L"等待 PotPlayer 回到前台；原画面已恢复");
                else fail(L"覆盖安全检查失败，已恢复原画面");
                return;
            }
            cropMismatches_=0;
            status_=L"视频区域捕获中 · 无增强对照 · 时间来源为屏幕合成器";
        } else if(now-openedAt_>10000) {
            fail(L"等待首帧超时，已恢复原画面");return;
        }
        if(now>=nextLog_) {
            const auto& f=state.metrics.flow;
            log::info("potplayer-attachment-stats",std::format(
                "session={} visible={} capture={}x{} realPresented={} submitFps={:.3f} sourceCompletedFps={:.3f} monitor={} dpi={} nr={} sr={} fg={} (submission is not physical display FPS)",
                state.sessionId,overlay_.visible(),state.sourceWidth,state.sourceHeight,f.counters.realPresented,
                f.presentSubmitFps,f.sourceCompletedFps,uint64_t(g.monitor),g.dpi,state.nrActive,state.srActive,state.fgActive));
            nextLog_=now+1000;
        }
        return;
    }
    if(active_.playerWindow||opening_)stopSession(L"窗口尺寸或屏幕变化，等待稳定后重新捕获");
    // Do not destroy/reuse an HWND still owned by the engine's old swapchain.
    if(!engine_.idle())return;
    if(!haveCandidate_||!sameCapture(candidate_,g)||!sameRect(candidate_.videoRect,g.videoRect)) {
        candidate_=g;stableSince_=now;haveCandidate_=true;return;
    }
    if(now-stableSince_<350)return;
    start(g);
}
}
