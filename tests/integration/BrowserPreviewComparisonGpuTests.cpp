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
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

// Synthetic, test-only GPU readback. This is neither a playback path nor a
// displayed-FPS/DWM-visibility measurement. No user media or SDK provider runs.
using namespace veyra;
namespace {
struct Deadline {
    std::mutex mutex;std::condition_variable cv;bool done=false;std::thread thread;
    Deadline():thread([this]{std::unique_lock lock(mutex);if(!cv.wait_for(lock,std::chrono::seconds(290),[this]{return done;})){
        std::cerr<<"BROWSER_COMPARE_TIMEOUT exceeded 290 seconds\n"<<std::flush;std::_Exit(124);
    }}){}
    ~Deadline(){{std::lock_guard lock(mutex);done=true;}cv.notify_all();thread.join();}
};
void pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
std::wstring folded(std::filesystem::path path){auto s=path.generic_wstring();std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return std::towlower(c);});return s;}
bool allowedOutput(const std::filesystem::path& path){
    if(!path.is_absolute())return false;
    const auto target=std::filesystem::weakly_canonical(path);const auto t=folded(target);
    for(const auto* root:{L"E:/\u9879\u76ee/Veyra/browser-stage10-20261014/tests",
                         L"E:/\u9879\u76ee/Veyra/beauty-stage11-20261014/tests",
                         L"E:/\u9879\u76ee/Veyra/visual-tuning-stage12-20261014/tests"}){
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
constexpr std::array<Color,4> colors{{{96,32,16},{32,96,16},{16,32,96},{96,96,96}}};
int exposureCode(int original){
    const double encoded=double(original)/255;
    const double linear=encoded<=.04045?encoded/12.92:std::pow((encoded+.055)/1.055,2.4);
    const double lit=linear*2;
    const double result=lit<=.0031308?lit*12.92:1.055*std::pow(lit,1/2.4)-.055;
    return std::clamp(int(std::lround(result*255)),0,255);
}
Color exposed(Color original){for(auto& channel:original)channel=exposureCode(channel);return original;}
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
    return std::abs(int(p[0])-wanted[0])<=3&&std::abs(int(p[1])-wanted[1])<=3&&std::abs(int(p[2])-wanted[2])<=3&&p[3]>=252;
}
bool pattern(const sink::RgbaImage& image,unsigned phase,int cw,int ch,int mode,float split){
    const double scale=std::min(double(cw)/64,double(ch)/32),rw=64*scale,rh=32*scale;
    const double left=(cw-rw)/2,top=(ch-rh)/2;
    bool pass=true;
    std::vector<double> xs{left+rw*.12,left+rw*.375,left+rw*.625,left+rw*.88};
    const double cut=double(cw)*.5+(double(split)-.5)*rw;
    if(mode==2&&cut-left>12&&left+rw-cut>12){xs.push_back(cut-8);xs.push_back(cut+8);}
    for(double x:xs)for(double fraction:{.18,.32,.68,.82}){
        const double y=top+rh*fraction;
        // A magnified source quadrant edge legitimately contains bilinear
        // mixtures. Pure-color assertions belong outside that filter footprint;
        // the wipe matrix below checks every edge pixel against independently
        // rendered original/enhanced images of this exact same source frame.
        if(std::abs(x-(left+rw*.5))<scale*.5+1)continue;
        const unsigned quadrant=(x>=left+rw*.5?1u:0u)+(fraction>=.5?2u:0u);
        const auto original=colors[(quadrant+phase)%4];
        const bool useOriginal=mode==1||(mode==2&&x<cut);
        pass=pass&&colorAt(image,x,y,cw,ch,useOriginal?original:exposed(original));
    }
    if(top>4){pass=pass&&colorAt(image,cw*.5,top*.5,cw,ch,{0,0,0});
        pass=pass&&colorAt(image,cw*.5,ch-top*.5,cw,ch,{0,0,0});}
    if(left>4){pass=pass&&colorAt(image,left*.5,ch*.5,cw,ch,{0,0,0});
        pass=pass&&colorAt(image,cw-left*.5,ch*.5,cw,ch,{0,0,0});}
    return pass;
}
bool identical(const sink::RgbaImage& a,const sink::RgbaImage& b){
    return a.width==b.width&&a.height==b.height&&a.pixels==b.pixels;
}
bool wipeMatchesRenderedSources(const sink::RgbaImage& wipe,const sink::RgbaImage& original,
                                const sink::RgbaImage& enhanced,float split,int cw,int ch,float dpiScale){
    if(!wipe.width||!wipe.height||wipe.width!=original.width||wipe.width!=enhanced.width||
       wipe.height!=original.height||wipe.height!=enhanced.height||
       wipe.pixels.size()!=size_t(wipe.width)*wipe.height*4||
       original.pixels.size()!=wipe.pixels.size()||enhanced.pixels.size()!=wipe.pixels.size())return false;
    const double fit=std::min(double(cw)/64,double(ch)/32),rw=64*fit,rh=32*fit;
    const double left=(cw-rw)/2,top=(ch-rh)/2;
    const double boundary=cw*.5+(double(split)-.5)*rw;
    const unsigned cut=unsigned(std::clamp(boundary*wipe.width/cw,0.0,double(wipe.width)));
    const bool guide=split>0&&split<1&&boundary>0&&boundary<cw;
    // Only the bounded display decoration can differ. Every pixel beyond its
    // four-DIP stroke / 20x28-DIP handle bounds, including filter mixtures and
    // letterbox bars, must still equal the independently rendered source.
    for(unsigned y=0;y<wipe.height;++y)for(unsigned x=0;x<wipe.width;++x){
        const double px=(x+.5)*cw/wipe.width,py=(y+.5)*ch/wipe.height;
        const bool inPicture=px>=left&&px<=left+rw&&py>=top&&py<=top+rh;
        const bool inGuideBounds=guide&&inPicture&&
            (std::abs(px-boundary)<2*dpiScale||
             (std::abs(px-boundary)<=10*dpiScale&&std::abs(py-ch*.5)<=14*dpiScale));
        if(inGuideBounds)continue;
        const size_t at=(size_t(y)*wipe.width+x)*4;
        const auto& expected=x<cut?original:enhanced;
        if(!std::equal(wipe.pixels.begin()+at,wipe.pixels.begin()+at+4,expected.pixels.begin()+at))return false;
    }
    return true;
}
bool guideHasContrast(const sink::RgbaImage& image,float split,int cw,int ch,float dpiScale){
    if(!(split>0&&split<1))return true;
    const double fit=std::min(double(cw)/64,double(ch)/32),rw=64*fit,rh=32*fit;
    const double boundary=cw*.5+(double(split)-.5)*rw,top=(ch-rh)/2;
    if(boundary<=10*dpiScale||boundary>=cw-10*dpiScale||rh<56*dpiScale)return true;
    // Independent visible feature checks: white stroke, black outline, white
    // grip fill, black border and its small grip mark. They remain visible on
    // both solid-black and solid-white video backgrounds.
    return colorAt(image,boundary,top+rh*.25,cw,ch,{255,255,255})&&
        colorAt(image,boundary+1.5*dpiScale,top+rh*.25,cw,ch,{0,0,0})&&
        colorAt(image,boundary+6*dpiScale,ch*.5,cw,ch,{255,255,255})&&
        colorAt(image,boundary+9.5*dpiScale,ch*.5,cw,ch,{0,0,0})&&
        colorAt(image,boundary+3*dpiScale,ch*.5,cw,ch,{0,0,0});
}
struct Fixture {
    HWND primary=nullptr,parent=nullptr,child=nullptr;
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;
    std::unique_ptr<pipeline::EnhanceGraph> graph;engine::VideoPresenter presenter;
    pipeline::EnhanceGraph::FrameOutputs out;AVFrame* frame=nullptr;
    bool gpuClosed=false;uint64_t frameId=0;unsigned processCalls=0,uploadCalls=0;unsigned phase=0;
    engine::BrowserPreviewRequest request{};uint64_t session=41;
    ~Fixture(){shutdown();}
    void shutdown(){
        if(!gpuClosed){if(ring.initialized())ring.drainQueue();out={};presenter.close();if(graph)graph->shutdown();
            graph.reset();av_frame_free(&frame);ring.shutdown();ctx.shutdown();gpuClosed=true;}
        if(child){DestroyWindow(child);child=nullptr;}if(parent){DestroyWindow(parent);parent=nullptr;}
        if(primary){DestroyWindow(primary);primary=nullptr;}
    }
    bool makeWindows(){
        const auto module=GetModuleHandleW(nullptr);
        primary=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Veyra comparison synthetic primary",WS_POPUP|WS_CLIPCHILDREN,20,20,320,240,nullptr,nullptr,module,nullptr);
        parent=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Veyra comparison synthetic preview",WS_POPUP|WS_CLIPCHILDREN,360,20,1280,720,nullptr,nullptr,module,nullptr);
        child=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Comparison preview child",WS_CHILD|WS_VISIBLE,0,0,1280,720,parent,nullptr,module,nullptr);
        if(!primary||!parent||!child)return false;
        ShowWindow(primary,SW_SHOWNOACTIVATE);ShowWindow(parent,SW_SHOWNOACTIVATE);pump();return true;
    }
    void select(int mode,float split=.5f){
        request.comparisonMode=mode;request.comparisonSplit=split;++request.revision;
        presenter.requestBrowserPreview(request,session,true);
    }
    bool next(unsigned nextPhase,bool references=true,bool reset=false){
        out={};phase=nextPhase;fill(frame,phase);++frameId;++processCalls;
        const bool ok=graph->process(frame,double(frameId)*1000/30,reset||frameId==1,out,frameId,nullptr,nullptr,references);
        if(ok)presenter.sourceProcessed(out.batch.identity);return ok;
    }
    bool nextSolid(Color color){
        out={};++frameId;++processCalls;
        for(int y=0;y<frame->height;++y)for(int x=0;x<frame->width;++x){
            auto* pixel=frame->data[0]+y*frame->linesize[0]+x*4;
            for(unsigned c=0;c<3;++c)pixel[c]=uint8_t(color[c]);pixel[3]=255;
        }
        const bool ok=graph->process(frame,double(frameId)*1000/30,false,out,frameId,nullptr,nullptr,true);
        if(ok)presenter.sourceProcessed(out.batch.identity);return ok;
    }
    bool present(){
        const bool references=out.batch.count&&out.batch.frames[out.batch.count-1].lease&&out.batch.frames[out.batch.count-1].lease->referencesValid;
        // Browser side comparison NEVER reaches the primary comparison argument.
        return presenter.present(ctx,ring,*graph,out.videoSlot,false,references,0,false,.5f,out.batch.identity,{},int64_t(frameId)*10000000/30);
    }
    bool settle(uint64_t expectedId,unsigned attempts=150){
        for(unsigned i=0;i<attempts;++i){pump();const auto s=presenter.browserPreviewSnapshot();
            if(s.failed)return false;if(s.hasFrame&&s.live&&s.sourceFrameId==expectedId&&s.frameRequestRevision==request.revision)return true;
            if(!present())return false;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        return false;
    }
};
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2){std::cerr<<"Usage: veyra_browser_preview_comparison_gpu_tests <Stage10/11/12 tests/output-directory>\n";return 2;}
    std::filesystem::path dir;
    try{dir=std::filesystem::path(argv[1]);if(!allowedOutput(dir)){std::cerr<<"Output must stay inside the allowed Stage10/11/12 tests directories\n";return 2;}
        std::filesystem::create_directories(dir);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
    Deadline deadline;Checks checks;bool debugAvailable=false;unsigned debugErrors=0;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);const bool comOwned=SUCCEEDED(com);
    Fixture f;gfx::ComPtr<ID3D12InfoQueue> info;
    Logger::instance().openFile((dir/L"engine.log").wstring());
    try{
        checks.require(comOwned,"initialize owned test COM apartment");checks.require(f.makeWindows(),"create visible owned noactivate child target");
        Status status=Status::Ok;gfx::DeviceContextDesc device;device.enableDebugLayer=true;
        checks.require(f.ctx.initialize(device,status),"initialize synthetic D3D12 context");debugAvailable=f.ctx.debugLayerEnabled();
        if(SUCCEEDED(f.ctx.device()->QueryInterface(IID_PPV_ARGS(&info))))info->ClearStoredMessages();
        checks.require(f.ring.initialize(f.ctx.device(),f.ctx.directQueue(),f.ctx.fence(),f.ctx.fenceEvent(),12,status),"initialize bounded shared producer/consumer ring");
        f.graph=std::make_unique<pipeline::EnhanceGraph>(f.ctx,f.ring);
        pipeline::EnhanceGraphDesc desc;desc.sourceWidth=desc.workWidth=64;desc.sourceHeight=desc.workHeight=32;
        desc.rgbInput=desc.noFeatures=true;desc.stillImage=false;desc.enableNr=desc.enableFg=desc.enableSr=false;desc.settingsRevision=11;
        desc.outputDitherStep=0;desc.color.enabled=true;desc.color.exposure=1;
        desc.stageMark=[&](const char* stage){if(std::string(stage)=="upload")++f.uploadCalls;};
        checks.require(f.graph->initialize(desc)&&f.presenter.open(f.ctx,f.primary,*f.graph)&&f.graph->createViews(),"initialize ONE real +1EV graph with no SDK features");
        engine::PresentationSettings settings;settings.display=engine::DisplaySync::Tearing;std::wstring settingsStatus;
        f.presenter.configurePresentation(f.ctx,settings,false,settingsStatus);
        f.frame=av_frame_alloc();checks.require(f.frame!=nullptr,"allocate synthetic RGBA frame");
        f.frame->format=AV_PIX_FMT_RGBA;f.frame->width=64;f.frame->height=32;
        f.frame->color_trc=AVCOL_TRC_IEC61966_2_1;f.frame->color_range=AVCOL_RANGE_JPEG;f.frame->colorspace=AVCOL_SPC_RGB;
        checks.require(av_frame_get_buffer(f.frame,32)>=0,"allocate synthetic pixels without user data");
        f.request.window=reinterpret_cast<uintptr_t>(f.child);f.request.sessionId=f.session;f.request.visible=true;f.select(0);
        auto primaryBaseline=[&]{
            checks.require(f.present(),"global comparison=0 presents enhanced primary");
            sink::RgbaImage image;checks.require(f.presenter.readPresentedFrameForTest(f.ctx,f.ring,image),"read actual primary baseline");
            checks.check(image.width==320&&image.height==240&&pattern(image,f.phase,320,240,0,.5f),"primary contains genuinely exposed synthetic colors");
            return image;
        };
        auto verify=[&](const sink::RgbaImage& baseline,int actualMode,float split,bool references,const char* name,int cw=1280,int ch=720){
            const auto calls=f.processCalls,uploads=f.uploadCalls;
            checks.require(f.present()&&f.settle(f.frameId),"matching mode revision reaches successful completed preview");
            sink::RgbaImage mirror,primary;
            checks.require(f.presenter.readBrowserPreviewFrameForTest(f.ctx,f.ring,mirror),"read actual secondary presented pixels");
            checks.check(mirror.width==1280&&mirror.height==720&&pattern(mirror,f.phase,cw,ch,actualMode,split),name);
            const float dpiScale=std::max(1.0f,float(GetDpiForWindow(f.child))/96.0f);
            if(actualMode==2)checks.check(guideHasContrast(mirror,split,cw,ch,dpiScale),
                "visible divider has white core, dark outline and a small high-contrast handle");
            checks.require(f.presenter.readPresentedFrameForTest(f.ctx,f.ring,primary),"read actual primary after optional comparison");
            checks.check(identical(primary,baseline),"ALL primary pixels unchanged by optional preview comparison");
            const auto s=f.presenter.browserPreviewSnapshot();
            checks.check(s.hasFrame&&s.live&&!s.failed&&s.window==f.request.window&&s.sessionId==f.session&&s.revision==f.request.revision&&
                s.frameRequestRevision==f.request.revision&&s.comparisonMode==actualMode&&std::abs(s.comparisonSplit-split)<.00001f&&
                s.sourceFrameId==f.frameId&&s.epoch==f.out.batch.identity.epoch&&s.settingsRevision==f.out.batch.identity.settingsRevision,
                "actual pixels publish exact target/frame/request/generation identity");
            checks.check(s.referenceAvailable==references,"reference availability reflects THIS real-frame lease");
            checks.check(f.processCalls==calls&&f.uploadCalls==uploads,"mode/split redraw does NOT process the source a second time");
            checks.check(sink::saveImage((dir/(std::string(name)+".png")).wstring(),mirror),"save synthetic comparison image evidence");
            return mirror;
        };
        checks.require(f.next(0),"process first real ColorGrade input with retained reference");
        auto baseline=primaryBaseline();auto enhanced=verify(baseline,0,.5f,true,"enhanced-only");
        const auto firstFrameRevision=f.presenter.browserPreviewSnapshot().frameRequestRevision;
        f.select(1);
        const auto changing=f.presenter.browserPreviewSnapshot();
        checks.check(changing.hasFrame&&!changing.live&&changing.frameRequestRevision==firstFrameRevision&&changing.comparisonMode==0,
            "presentation-only change keeps previous actual image without claiming new mode ready");
        auto original=verify(baseline,1,.5f,true,"original-only");
        checks.check(!identical(original,enhanced),"actual ORIGINAL and ENHANCED images are visibly different");
        for(float split:{0.0f,.25f,.5f,1.0f}){
            f.select(2,split);const std::string name=std::string("wipe-")+std::to_string(int(split*100));
            auto wipe=verify(baseline,2,split,true,name.c_str());
            checks.check(wipeMatchesRenderedSources(wipe,original,enhanced,split,1280,720,
                std::max(1.0f,float(GetDpiForWindow(f.child))/96.0f)),
                "every pixel outside the bounded divider matches its actual original/enhanced draw");
            if(split==0)checks.check(identical(wipe,enhanced),"wipe=0 is exactly enhanced-only");
            if(split==1)checks.check(identical(wipe,original),"wipe=1 is exactly original-only");
        }
        // Alternate real source parity and color content. A previous slot's
        // ungraded texture is different, so stale-reference use cannot pass.
        for(unsigned i=1;i<=6;++i){
            checks.require(f.next(i%4),"rotate producer slots with a different source quadrant pattern");
            baseline=primaryBaseline();f.select(2,.25f);
            const std::string name=std::string("slot-reference-")+std::to_string(i);
            verify(baseline,2,.25f,true,name.c_str());
        }
        // No reference writes are performed for this new source frame. Existing
        // old textures remain allocated: only the true referencesValid authority
        // may decide whether they can be drawn.
        f.select(1,.5f);checks.require(f.next(3,false),"process current source with reference retention explicitly OFF");
        checks.check(f.out.batch.count&&f.out.batch.frames[f.out.batch.count-1].lease&&!f.out.batch.frames[f.out.batch.count-1].lease->referencesValid,
            "fixture genuinely has unavailable references rather than fake metadata");
        baseline=primaryBaseline();auto unavailableOriginal=verify(baseline,0,.5f,false,"unavailable-original-falls-back-current-enhanced");
        f.select(2,.25f);auto unavailableWipe=verify(baseline,0,.25f,false,"unavailable-wipe-falls-back-current-enhanced");
        checks.check(identical(unavailableOriginal,unavailableWipe),"invalid references produce no divider pixels anywhere");
        checks.check(f.request.comparisonMode==2,"fallback does not overwrite the requested comparison mode");
        checks.require(f.next(1),"next valid reference frame resumes the requested comparison");
        baseline=primaryBaseline();verify(baseline,2,.25f,true,"valid-reference-restores-wipe");
        // A submitted old-revision image may finish after a new request. It
        // cannot relabel the image as the newly requested mode.
        f.select(1,.5f);checks.require(f.present(),"enqueue an original request before a newer mode");
        const auto previousActual=f.presenter.browserPreviewSnapshot();
        f.select(2,.75f);checks.require(f.ring.drainQueue(),"complete queued old mode commands after new request");
        const auto pending=f.presenter.browserPreviewSnapshot();
        checks.check(!pending.live&&pending.frameRequestRevision!=f.request.revision&&
            pending.comparisonMode==previousActual.comparisonMode&&pending.comparisonSplit==previousActual.comparisonSplit,
            "old completion cannot claim the new requested wipe pixels");
        verify(baseline,2,.75f,true,"matching-revision-restores-live");
        auto invalid=f.request;invalid.comparisonMode=1;
        f.presenter.requestBrowserPreview(invalid,f.session,true);
        const auto sameRevision=f.presenter.browserPreviewSnapshot();
        checks.check(!sameRevision.live&&sameRevision.comparisonMode==2,"same revision cannot alter comparison identity");
        auto stale=f.request;--stale.revision;f.presenter.requestBrowserPreview(stale,f.session,true);
        checks.check(!f.presenter.browserPreviewSnapshot().live,"descending request revision cannot become live");
        f.presenter.requestBrowserPreview(f.request,f.session+1,true);
        checks.check(!f.presenter.browserPreviewSnapshot().live,"wrong active session cannot authorize a comparison frame");
        f.presenter.requestBrowserPreview(f.request,f.session,true);verify(baseline,2,.75f,true,"correct-active-session-restored");
        ++f.session;f.request.sessionId=f.session;f.select(1,.5f);
        const auto newSession=f.presenter.browserPreviewSnapshot();
        checks.check(!newSession.hasFrame&&!newSession.live&&newSession.frameRequestRevision==0,
            "new session immediately revokes old visible-frame authority");
        verify(baseline,1,.5f,true,"new-session-matching-current-source");
        auto rejectOldGeneration=[&](bool resetEpoch,const char* label){
            auto previous=std::move(f.out);const auto previousIdentity=previous.batch.identity;
            if(resetEpoch)f.presenter.invalidateBrowserPreviewGeneration();
            else {engine::EnhancementSettings live;live.revision=previousIdentity.settingsRevision+1;live.color=desc.color;
                checks.require(f.graph->applySettings(live),"apply actual graph settings revision with exposure retained");}
            checks.require(f.next(2,true,resetEpoch),"process the genuine next graph epoch or settings revision");
            checks.check(resetEpoch?f.out.batch.identity.epoch!=previousIdentity.epoch:f.out.batch.identity.settingsRevision!=previousIdentity.settingsRevision,
                "producer graph generation truly changes");
            checks.require(f.presenter.present(f.ctx,f.ring,*f.graph,previous.videoSlot,false,true,0,false,.5f,previousIdentity),
                "old leased primary draw is safe while optional preview rejects its generation");
            checks.require(f.ring.drainQueue(),"finish stale generation commands before inspecting preview");
            const auto revoked=f.presenter.browserPreviewSnapshot();checks.check(!revoked.hasFrame&&!revoked.live,label);
            previous={};baseline=primaryBaseline();verify(baseline,1,.5f,true,resetEpoch?"new-epoch-reference":"new-settings-reference");
        };
        rejectOldGeneration(true,"old epoch cannot publish source/reference pixels");
        rejectOldGeneration(false,"old settings revision cannot publish source/reference pixels");
        SetWindowPos(f.parent,nullptr,0,0,800,800,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(f.child,nullptr,0,0,800,800,SWP_NOZORDER|SWP_NOACTIVATE);pump();
        f.select(2,.5f);verify(baseline,2,.5f,true,"square-child-source-and-enhanced-same-contain",800,800);
        // Validate current-client guide geometry on a retained 1280x720 buffer,
        // including pillarboxing where the cut is NOT simply clientWidth*split.
        SetWindowPos(f.parent,nullptr,0,0,1600,600,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(f.child,nullptr,0,0,1600,600,SWP_NOZORDER|SWP_NOACTIVATE);pump();
        f.select(0,.25f);auto wideEnhanced=verify(baseline,0,.25f,true,"wide-child-enhanced",1600,600);
        f.select(1,.25f);auto wideOriginal=verify(baseline,1,.25f,true,"wide-child-original",1600,600);
        f.select(2,.25f);auto wideWipe=verify(baseline,2,.25f,true,"wide-child-divider-current-client-mapping",1600,600);
        checks.check(wipeMatchesRenderedSources(wideWipe,wideOriginal,wideEnhanced,.25f,1600,600,
            std::max(1.0f,float(GetDpiForWindow(f.child))/96.0f)),
            "retained wide client changes only its narrow guide, with exact sides and black bars");
        SetWindowPos(f.parent,nullptr,0,0,1280,720,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(f.child,nullptr,0,0,1280,720,SWP_NOZORDER|SWP_NOACTIVATE);pump();
        for(const auto& background:std::array<Color,2>{{{0,0,0},{255,255,255}}}){
            checks.require(f.nextSolid(background),"process a real solid black/white frame with valid references");
            checks.require(f.present(),"present the genuine solid-color graph frame");
            sink::RgbaImage solidPrimary;checks.require(f.presenter.readPresentedFrameForTest(f.ctx,f.ring,solidPrimary),"read solid-color primary baseline");
            auto readSolid=[&](int mode){
                f.select(mode,.5f);checks.require(f.present()&&f.settle(f.frameId),"complete solid-color secondary mode");
                sink::RgbaImage image;checks.require(f.presenter.readBrowserPreviewFrameForTest(f.ctx,f.ring,image),"read actual solid-color comparison pixels");
                return image;
            };
            const auto solidEnhanced=readSolid(0),solidOriginal=readSolid(1),solidWipe=readSolid(2);
            const float dpiScale=std::max(1.0f,float(GetDpiForWindow(f.child))/96.0f);
            checks.check(guideHasContrast(solidWipe,.5f,1280,720,dpiScale),
                "divider remains visible against this solid black/white background");
            checks.check(wipeMatchesRenderedSources(solidWipe,solidOriginal,solidEnhanced,.5f,1280,720,dpiScale),
                "solid-color guide changes no pixels outside its narrow display bounds");
            sink::RgbaImage primary;checks.require(f.presenter.readPresentedFrameForTest(f.ctx,f.ring,primary),"read solid primary after secondary guide");
            checks.check(identical(primary,solidPrimary),"secondary guide leaves the entire primary solid frame unchanged");
        }
        f.request.visible=false;++f.request.revision;ShowWindow(f.child,SW_HIDE);f.presenter.requestBrowserPreview(f.request,f.session,true);
        checks.require(f.next(3)&&f.present(),"hidden comparison preview leaves primary running");
        sink::RgbaImage primary;checks.require(f.presenter.readPresentedFrameForTest(f.ctx,f.ring,primary),"read primary with secondary hidden");
        checks.check(pattern(primary,3,320,240,0,.5f)&&!f.presenter.browserPreviewSnapshot().live,"hidden target preserves enhanced primary and never claims live preview");
        const auto& metrics=f.graph->metrics();
        checks.check(metrics.nrEvaluateCount==0&&metrics.srEvaluateCount==0&&metrics.fgGeneratedFrames==0&&metrics.nvofExecuteCount==0,
            "synthetic fixture evaluated no AI or frame-generation provider");
        checks.check(f.uploadCalls==f.processCalls,"source upload occurs exactly once per deliberate source process");
        checks.require(f.ring.drainQueue(),"finish GPU reads before resource teardown");f.out={};f.presenter.close();f.presenter.close();
        checks.check(!f.presenter.browserPreviewSnapshot().hasFrame,"ordered idempotent presenter close revokes all images");
        f.graph->shutdown();f.graph.reset();
        if(info){
            for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;
                if(FAILED(info->GetMessage(i,nullptr,&size)))continue;std::vector<char> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if(SUCCEEDED(info->GetMessage(i,message,&size))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++debugErrors;std::cerr<<"DEBUG_ERROR "<<message->pDescription<<std::endl;}}
            checks.check(debugErrors==0,"no D3D12 debug-layer state or resource lifetime errors");
        }else std::cout<<"UNCOVERED D3D12 info queue unavailable; no debug-layer pass claim\n";
        checks.check(SUCCEEDED(f.ctx.device()->GetDeviceRemovedReason()),"GPU device remains healthy after comparison teardown");
    }catch(const std::exception& e){std::cerr<<"BROWSER_COMPARE_EXCEPTION "<<e.what()<<std::endl;if(checks.failures==0)++checks.failures;}
    info.Reset();f.shutdown();if(comOwned)CoUninitialize();
    std::ofstream result(dir/L"result.json");
    result<<"{\"schema\":1,\"syntheticGpuComparisonChecksPassed\":"<<(checks.failures==0?"true":"false")
          <<",\"checks\":"<<checks.checks<<",\"failures\":"<<checks.failures<<",\"debugLayerAvailable\":"<<(debugAvailable?"true":"false")
          <<",\"debugErrors\":"<<debugErrors<<",\"sourceProcessCalls\":"<<f.processCalls<<",\"sourceUploads\":"<<f.uploadCalls
          <<",\"dwmVisibleAcceptance\":false,\"displayFpsMeasured\":false,\"aiProviderEffectsTested\":false,\"inputTransform\":\"existing ColorGrade +1EV\"}\n";
    std::cout<<"BROWSER_COMPARE_GPU "<<(checks.failures?"FAIL":"PASS")<<" checks="<<checks.checks<<" failures="<<checks.failures<<std::endl;
    return checks.failures?1:0;
}
