#include "veyra/engine/VideoPresenter.h"
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/sink/ImageExportSink.h"
#include "veyra/Log.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

// Synthetic integration diagnostics ONLY. Readback is not a playback path,
// a DWM visibility assertion, or displayed-FPS evidence. No input injection,
// user media, capture, NR/SR/FG provider, or settings file is used here.
using namespace veyra;
namespace {
using Clock=std::chrono::steady_clock;
struct Deadline {
    std::mutex mutex;std::condition_variable cv;bool done=false;std::thread thread;
    Deadline():thread([this]{std::unique_lock lock(mutex);if(!cv.wait_for(lock,std::chrono::seconds(290),[this]{return done;})){
        std::cerr<<"BROWSER_PREVIEW_TIMEOUT own test exceeded 290 seconds\n"<<std::flush;std::_Exit(124);
    }}){}
    ~Deadline(){{std::lock_guard lock(mutex);done=true;}cv.notify_all();thread.join();}
};
void pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
std::wstring folded(std::filesystem::path path){auto s=path.generic_wstring();std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return std::towlower(c);});return s;}
bool allowedOutput(const std::filesystem::path& path){
    if(!path.is_absolute())return false;
    const auto target=std::filesystem::weakly_canonical(path);const auto t=folded(target);
    for (const auto* root : {L"E:/\u9879\u76ee/Veyra/browser-stage9-20261014/tests",
                            L"E:/\u9879\u76ee/Veyra/browser-stage10-20261014/tests",
                            L"E:/\u9879\u76ee/Veyra/beauty-stage11-20261014/tests"}) {
        const auto b=folded(std::filesystem::weakly_canonical(std::filesystem::path(root)));
        if(t==b||(t.size()>b.size()&&t.compare(0,b.size(),b)==0&&t[b.size()]==L'/'))return true;
    }
    return false;
}
struct Checks {
    unsigned checks=0,failures=0;
    bool check(bool value,const char* label){++checks;failures+=!value;std::cout<<(value?"PASS ":"FAIL ")<<label<<std::endl;return value;}
    void require(bool value,const char* label){if(!check(value,label))throw std::runtime_error(label);}
};
using Color=std::array<int,3>;
constexpr std::array<Color,4> colors{{{255,0,0},{0,255,0},{255,0,255},{255,255,255}}};
void fill(AVFrame* frame,unsigned phase){
    for(int y=0;y<frame->height;++y)for(int x=0;x<frame->width;++x){
        const unsigned quadrant=(x>=frame->width/2?1u:0u)+(y>=frame->height/2?2u:0u);
        const auto& color=colors[(quadrant+phase)%4];auto* pixel=frame->data[0]+y*frame->linesize[0]+x*4;
        for(unsigned c=0;c<3;++c)pixel[c]=uint8_t(color[c]);pixel[3]=255;
    }
}
bool colorAt(const sink::RgbaImage& image,double clientX,double clientY,int cw,int ch,const Color& wanted){
    if(!image.width||!image.height||image.pixels.size()!=size_t(image.width)*image.height*4)return false;
    const int x=std::clamp(int(clientX*image.width/cw),0,int(image.width)-1);
    const int y=std::clamp(int(clientY*image.height/ch),0,int(image.height)-1);
    const auto* p=image.pixels.data()+(size_t(y)*image.width+x)*4;
    return std::abs(int(p[0])-wanted[0])<=3&&std::abs(int(p[1])-wanted[1])<=3&&std::abs(int(p[2])-wanted[2])<=3;
}
bool pattern(const sink::RgbaImage& image,unsigned phase,int cw,int ch){
    const double scale=std::min(double(cw)/64,double(ch)/32),rw=64*scale,rh=32*scale;
    const double left=(cw-rw)/2,top=(ch-rh)/2;
    bool pass=true;
    // Interior quadrant centers plus all four picture corners expose clipping,
    // horizontal/vertical inversion, stale pixels, and partial draws.
    for(const double edge:{.12,.25})for(unsigned q=0;q<4;++q){
        const double x=left+rw*((q&1)?1-edge:edge),y=top+rh*((q&2)?1-edge:edge);
        pass=pass&&colorAt(image,x,y,cw,ch,colors[(q+phase)%4]);
    }
    if(top>4){pass=pass&&colorAt(image,cw*.5,top*.5,cw,ch,{0,0,0});
        pass=pass&&colorAt(image,cw*.5,ch-top*.5,cw,ch,{0,0,0});}
    // Measure the actual picture span in the fixed buffer, then map it to
    // CURRENT child coordinates. A square child intentionally yields a
    // non-square raw-buffer picture due to DXGI stretch precompensation.
    int first=int(image.height),last=-1;const unsigned scanX=image.width/4;
    for(unsigned y=0;y<image.height;++y){const auto* p=image.pixels.data()+(size_t(y)*image.width+scanX)*4;
        if(std::max({p[0],p[1],p[2]})>32){first=std::min(first,int(y));last=int(y);}}
    if(last<first)return false;
    const double actualHeight=double(last-first+1)*ch/image.height;
    return pass&&std::abs(actualHeight-rh)<=double(ch)*4/image.height&&std::abs(rw/actualHeight-2.0)<.035;
}
struct Fixture {
    HWND primary=nullptr,parent=nullptr,child=nullptr;
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;
    std::unique_ptr<pipeline::EnhanceGraph> graph;engine::VideoPresenter presenter;
    pipeline::EnhanceGraph::FrameOutputs out;AVFrame* frame=nullptr;
    bool gpuClosed=false;uint64_t frameId=0;
    ~Fixture(){shutdown();}
    void shutdown(){
        if(!gpuClosed){if(ring.initialized())ring.drainQueue();out={};presenter.close();if(graph)graph->shutdown();
            graph.reset();av_frame_free(&frame);ring.shutdown();ctx.shutdown();gpuClosed=true;}
        if(child){DestroyWindow(child);child=nullptr;}if(parent){DestroyWindow(parent);parent=nullptr;}
        if(primary){DestroyWindow(primary);primary=nullptr;}
    }
    bool makeWindows(){
        const auto module=GetModuleHandleW(nullptr);
        primary=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Veyra synthetic primary",WS_POPUP|WS_CLIPCHILDREN,20,20,320,240,nullptr,nullptr,module,nullptr);
        parent=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Veyra synthetic browser preview",WS_POPUP|WS_CLIPCHILDREN,360,20,1280,720,nullptr,nullptr,module,nullptr);
        child=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Synthetic preview child",WS_CHILD|WS_VISIBLE,0,0,1280,720,parent,nullptr,module,nullptr);
        if(!primary||!parent||!child)return false;
        ShowWindow(primary,SW_SHOWNOACTIVATE);ShowWindow(parent,SW_SHOWNOACTIVATE);pump();return true;
    }
    bool next(unsigned phase,bool reset=false){
        out={};fill(frame,phase);++frameId;
        const bool ok=graph->process(frame,double(frameId)*1000/30,reset||frameId==1,out,frameId);
        if(ok)presenter.sourceProcessed(out.batch.identity);return ok;
    }
    bool present(){
        const bool references=out.batch.count&&out.batch.frames[out.batch.count-1].lease&&out.batch.frames[out.batch.count-1].lease->referencesValid;
        return presenter.present(ctx,ring,*graph,out.videoSlot,false,references,0,false,.5f,out.batch.identity,{},int64_t(frameId)*10000000/30);
    }
    bool settle(uint64_t expectedId,unsigned attempts=100){
        for(unsigned i=0;i<attempts;++i){pump();const auto s=presenter.browserPreviewSnapshot();
            if(s.failed)return false;if(s.hasFrame&&s.live&&s.sourceFrameId==expectedId)return true;
            if(!present())return false;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        return false;
    }
};
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2){std::cerr<<"Usage: veyra_browser_preview_gpu_tests <Stage9/10/11 tests/output-directory>\n";return 2;}
    std::filesystem::path dir;
    try{dir=std::filesystem::path(argv[1]);if(!allowedOutput(dir)){std::cerr<<"Output must stay inside the allowed Stage9/10/11 tests directories\n";return 2;}
        std::filesystem::create_directories(dir);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
    Deadline deadline;Checks checks;bool busyCovered=false,debugAvailable=false;unsigned debugErrors=0;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);const bool comOwned=SUCCEEDED(com);
    Fixture f;gfx::ComPtr<ID3D12InfoQueue> info;
    Logger::instance().openFile((dir/L"engine.log").wstring());
    try{
        checks.require(comOwned,"initialize test COM apartment");checks.require(f.makeWindows(),"own noactivate windows and visible WS_CHILD preview");
        Status status=Status::Ok;gfx::DeviceContextDesc device;device.enableDebugLayer=true;
        checks.require(f.ctx.initialize(device,status),"initialize synthetic GPU context");debugAvailable=f.ctx.debugLayerEnabled();
        if(SUCCEEDED(f.ctx.device()->QueryInterface(IID_PPV_ARGS(&info))))info->ClearStoredMessages();
        checks.require(f.ring.initialize(f.ctx.device(),f.ctx.directQueue(),f.ctx.fence(),f.ctx.fenceEvent(),12,status),"initialize one owned producer/presentation fence timeline");
        f.graph=std::make_unique<pipeline::EnhanceGraph>(f.ctx,f.ring);
        pipeline::EnhanceGraphDesc desc;desc.sourceWidth=desc.workWidth=64;desc.sourceHeight=desc.workHeight=32;
        desc.rgbInput=desc.noFeatures=true;desc.stillImage=false;desc.enableNr=desc.enableFg=false;desc.settingsRevision=11;
        checks.require(f.graph->initialize(desc)&&f.presenter.open(f.ctx,f.primary,*f.graph)&&f.graph->createViews(),"open one noFeatures graph and primary presenter");
        engine::PresentationSettings settings;settings.display=engine::DisplaySync::Tearing;std::wstring settingsStatus;
        f.presenter.configurePresentation(f.ctx,settings,false,settingsStatus);
        f.frame=av_frame_alloc();checks.require(f.frame!=nullptr,"allocate own RGBA frame");
        f.frame->format=AV_PIX_FMT_RGBA;f.frame->width=64;f.frame->height=32;f.frame->color_trc=AVCOL_TRC_IEC61966_2_1;
        checks.require(av_frame_get_buffer(f.frame,32)>=0,"allocate own synthetic frame pixels");
        constexpr uint64_t session=41;engine::BrowserPreviewRequest request{reinterpret_cast<uintptr_t>(f.child),session,1,true};
        f.presenter.requestBrowserPreview(request,session,true);
        auto verify=[&](unsigned phase,int cw,int ch,const char* name,bool save){
            checks.require(f.next(phase)&&f.present()&&f.settle(f.frameId),"present matching synthetic frame to both targets");
            sink::RgbaImage mirror,primary;
            checks.require(f.presenter.readBrowserPreviewFrameForTest(f.ctx,f.ring,mirror),"read actual successful secondary backbuffer");
            checks.check(mirror.width==1280&&mirror.height==720&&pattern(mirror,phase,cw,ch),name);
            checks.require(f.presenter.readPresentedFrameForTest(f.ctx,f.ring,primary),"read actual primary backbuffer");
            checks.check(primary.width==320&&primary.height==240&&pattern(primary,phase,320,240),"primary colors and contain remain correct");
            const auto s=f.presenter.browserPreviewSnapshot();checks.check(s.hasFrame&&s.live&&!s.failed&&s.window==request.window&&s.sessionId==request.sessionId&&s.revision==request.revision&&s.sourceFrameId==f.frameId&&s.epoch==f.out.batch.identity.epoch&&s.settingsRevision==f.out.batch.identity.settingsRevision,"completed frame carries current target session revision and identity");
            if(save)checks.check(sink::saveImage((dir/(std::string(name)+".png")).wstring(),mirror),"save synthetic image evidence");
            return mirror;
        };
        auto last=verify(0,1280,720,"initial-four-corners",true);
        for(unsigned n=1;n<=12;++n)last=verify(n%4,1280,720,"continuous-slot-rotation",n==12);
        // No readback/drain in this short burst: exercise real waitable capacity
        // refusal. Lack of a refusal is UNCOVERED, not a synthetic busy PASS.
        const auto busyBefore=f.presenter.browserPreviewSnapshot().skippedBusy;
        for(unsigned n=0;n<48;++n){checks.require(f.next(n%4)&&f.present(),"primary continues during unsynchronized secondary burst");pump();}
        const auto busyAfter=f.presenter.browserPreviewSnapshot();busyCovered=busyAfter.skippedBusy>busyBefore;
        std::cout<<(busyCovered?"PASS ":"UNCOVERED ")<<"secondary busy capacity refusal observed skipped="<<(busyAfter.skippedBusy-busyBefore)<<std::endl;
        checks.check(!busyAfter.failed,"secondary burst does not fail optional sink");
        // An explicit reset invalidates completed/pending old generations even
        // before another sourceProcessed callback can restore authority.
        f.presenter.invalidateBrowserPreviewGeneration();checks.require(f.ring.drainQueue(),"complete previously submitted burst after invalidation");
        checks.check(!f.presenter.browserPreviewSnapshot().hasFrame&&!f.presenter.browserPreviewSnapshot().live,"old completion cannot relight invalidated generation");
        last=verify(1,1280,720,"post-busy-latest-frame",true);
        auto rejectOldGeneration=[&](bool resetEpoch,const char* label){
            auto previous=std::move(f.out);const auto previousIdentity=previous.batch.identity;
            if(resetEpoch)f.presenter.invalidateBrowserPreviewGeneration();
            else {engine::EnhancementSettings live;live.revision=previousIdentity.settingsRevision+1;
                checks.require(f.graph->applySettings(live),"apply genuine noFeatures live settings revision");}
            checks.require(f.next(2,resetEpoch),"process actual next producer epoch or settings revision");
            checks.check(resetEpoch?f.out.batch.identity.epoch!=previousIdentity.epoch:f.out.batch.identity.settingsRevision!=previousIdentity.settingsRevision,"producer generation truly changes");
            checks.require(f.presenter.present(f.ctx,f.ring,*f.graph,previous.videoSlot,false,true,0,false,.5f,previousIdentity),"old-generation primary draw remains safe while secondary rejects it");
            checks.require(f.ring.drainQueue(),"finish old-generation command list before rejection observation");
            const auto invalidated=f.presenter.browserPreviewSnapshot();
            checks.check(!invalidated.hasFrame&&!invalidated.live,label);previous={};
            checks.require(f.present()&&f.settle(f.frameId),"current generation restores secondary actual pixels");
            sink::RgbaImage restored;checks.require(f.presenter.readBrowserPreviewFrameForTest(f.ctx,f.ring,restored),"read current generation after stale rejection");
            checks.check(pattern(restored,2,1280,720),"recovered generation contains the current synthetic image");
        };
        rejectOldGeneration(true,"same-session old epoch cannot publish or relight preview");
        rejectOldGeneration(false,"same-session old settings revision cannot publish or relight preview");
        last=verify(1,1280,720,"after-generation-recovery",true);
        const auto frozenId=f.presenter.browserPreviewSnapshot().sourceFrameId;
        f.presenter.requestBrowserPreview(request,session,false);
        checks.require(f.present(),"primary still renders retained current source when preview playing is false");
        sink::RgbaImage frozen;checks.require(f.presenter.readBrowserPreviewFrameForTest(f.ctx,f.ring,frozen),"read frozen secondary pixels");
        const auto paused=f.presenter.browserPreviewSnapshot();checks.check(frozen.pixels==last.pixels&&paused.hasFrame&&!paused.live&&paused.sourceFrameId==frozenId&&paused.state==engine::BrowserPreviewState::Frozen,"pause freezes actual last successful image without keeping graph lease");
        request.visible=false;++request.revision;ShowWindow(f.child,SW_HIDE);f.presenter.requestBrowserPreview(request,session,true);
        checks.require(f.next(3)&&f.present(),"hidden secondary cannot stop primary");
        checks.require(f.presenter.readBrowserPreviewFrameForTest(f.ctx,f.ring,frozen),"hidden retained backbuffer remains readable for test");
        checks.check(frozen.pixels==last.pixels&&!f.presenter.browserPreviewSnapshot().live,"hidden request does not overwrite frozen pixels");
        ShowWindow(f.child,SW_SHOWNOACTIVATE);request.visible=true;++request.revision;f.presenter.requestBrowserPreview(request,session,true);
        last=verify(2,1280,720,"re-enabled-new-revision",true);
        const auto accepted=request;auto stale=request;--stale.revision;f.presenter.requestBrowserPreview(stale,session,true);
        checks.require(f.next(3)&&f.present(),"stale request cannot fail primary");
        const auto rejected=f.presenter.browserPreviewSnapshot();checks.check(rejected.revision==accepted.revision&&!rejected.live,"stale target revision cannot become live");
        f.presenter.requestBrowserPreview(accepted,session+1,true);
        checks.require(f.next(0)&&f.present(),"mismatched session cannot fail primary");
        checks.check(!f.presenter.browserPreviewSnapshot().live,"wrong active session cannot publish live preview");
        f.presenter.requestBrowserPreview(accepted,session,true);last=verify(3,1280,720,"matching-session-restored",true);
        auto invalid=request;invalid.window=reinterpret_cast<uintptr_t>(f.primary);++invalid.revision;
        f.presenter.requestBrowserPreview(invalid,session,true);checks.require(f.next(1)&&f.present(),"non-child target never fails primary");
        sink::RgbaImage primary;checks.require(f.presenter.readPresentedFrameForTest(f.ctx,f.ring,primary),"read primary after invalid target");
        checks.check(pattern(primary,1,320,240)&&!f.presenter.browserPreviewSnapshot().live,"invalid secondary target rejects safely with correct primary pixels");
        request.revision=invalid.revision+1;f.presenter.requestBrowserPreview(request,session,true);
        SetWindowPos(f.parent,nullptr,0,0,800,800,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(f.child,nullptr,0,0,800,800,SWP_NOZORDER|SWP_NOACTIVATE);pump();
        last=verify(0,800,800,"square-child-compensated-contain",true);
        // Ordered close occurs while all borrowed HWNDs are still valid. A
        // repeated close must not use released RTVs, fences or graph resources.
        checks.require(f.ring.drainQueue(),"drain synthetic shared queue before close");f.out={};
        f.presenter.close();f.presenter.close();checks.check(!f.presenter.browserPreviewSnapshot().hasFrame,"idempotent close clears preview state");
        f.graph->shutdown();f.graph.reset();
        if(info){
            for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;
                if(FAILED(info->GetMessage(i,nullptr,&size)))continue;std::vector<char> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if(SUCCEEDED(info->GetMessage(i,message,&size))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++debugErrors;std::cerr<<"DEBUG_ERROR "<<message->pDescription<<std::endl;}}
            checks.check(debugErrors==0,"no debug-layer resource-state or lifetime errors");
        }else std::cout<<"UNCOVERED D3D12 info queue unavailable; no debug-layer pass claim\n";
        checks.check(SUCCEEDED(f.ctx.device()->GetDeviceRemovedReason()),"synthetic device remains healthy after ordered close");
    }catch(const std::exception& e){std::cerr<<"BROWSER_PREVIEW_EXCEPTION "<<e.what()<<std::endl;if(checks.failures==0)++checks.failures;}
    info.Reset();f.shutdown();if(comOwned)CoUninitialize();
    std::ofstream result(dir/L"result.json");
    result<<"{\"schema\":1,\"syntheticGpuImageChecksPassed\":"<<(checks.failures==0?"true":"false")
          <<",\"checks\":"<<checks.checks<<",\"failures\":"<<checks.failures<<",\"busyCovered\":"<<(busyCovered?"true":"false")
          <<",\"debugLayerAvailable\":"<<(debugAvailable?"true":"false")<<",\"debugErrors\":"<<debugErrors
          <<",\"dwmVisibleAcceptance\":false,\"displayFpsMeasured\":false,\"providerEffectsTested\":false}\n";
    std::cout<<"BROWSER_PREVIEW_RESULT imagePass="<<(checks.failures==0)<<" checks="<<checks.checks<<" failures="<<checks.failures<<" busyCovered="<<busyCovered<<" debugErrors="<<debugErrors<<std::endl;
    return checks.failures?1:0;
}
