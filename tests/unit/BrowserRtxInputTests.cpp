#include "veyra/engine/BrowserRtxInputPolicy.h"
#include "veyra/engine/GraphDescription.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <utility>

using namespace veyra;
using namespace veyra::engine;
using pipeline::Extent;
namespace {
unsigned checks=0,failures=0;
void check(bool ok,const char* label){
    ++checks;if(!ok){++failures;std::cerr<<"FAIL "<<label<<'\n';}
}
EnhancementSettings settings(){
    EnhancementSettings s;s.sr=true;s.nr=false;s.lowLatency=false;
    s.videoSrQuality=4;s.videoSrInputExtent={1920,1080};
    s.srPlaybackExtent={3840,2160};return s;
}
StageRequest request(Extent raw={2560,1440}){
    StageRequest r;r.width=raw.width;r.height=raw.height;r.sr=true;return r;
}
void policy(){
    check(browserRtxInputExtent({3840,2160},{1920,1080})==Extent{1920,1080},"4K compositor uses known 1080p budget");
    check(browserRtxInputExtent({2160,3840},{1080,1920})==Extent{1080,1920},"portrait follows source orientation");
    check(browserRtxInputExtent({3840,1600},{1920,800})==Extent{1920,800},"cinema ratio remains unchanged");
    check(browserRtxInputExtent({1920,1080},{1920,1080})==Extent{},"native-size capture retains full pixels");
    check(browserRtxInputExtent({1280,720},{1920,1080})==Extent{},"small viewport never enlarges VSR input");
    check(browserRtxInputExtent({3244,1744},{1920,1080})==Extent{},"different aspect conservatively retains full capture");
    check(browserRtxInputExtent({3840,2160},{1080,1920})==Extent{},"stale orientation is rejected");
    check(browserRtxInputExtent({0,2160},{1920,1080})==Extent{},"missing capture is rejected");
    check(browserRtxInputExtent({3840,2160},{1920,0})==Extent{},"missing original is rejected");
    check(browserRtxInputExtent({3840,2160},{63,36})==Extent{},"sub-64 metadata is rejected");
    check(browserRtxInputExtent({16385,9216},{1920,1080})==Extent{},"capture beyond D3D extent is rejected");
    check(browserRtxInputExtent({3840,2160},{std::numeric_limits<uint32_t>::max(),1080})==Extent{},"overflow-like metadata is rejected");
    const auto rounded=browserRtxInputExtent({3244,1825},{1920,1080});
    check(rounded.valid()&&rounded.width<=1920&&rounded.height<=1080&&
        rounded.width%2==0&&rounded.height%2==0&&browserRtxInputAspectMatches(rounded,{3244,1825}),
        "rounded browser geometry produces bounded even input with matching aspect");
    for(const auto capture:{Extent{3840,2160},Extent{2560,1440},Extent{1920,1080},Extent{2160,3840},Extent{3840,1600}}){
        for(const auto original:{Extent{1920,1080},Extent{1280,720},Extent{1080,1920},Extent{1920,800},Extent{640,360}}){
            const auto hint=browserRtxInputExtent(capture,original);
            check(hint==Extent{}||(hint.width>=64&&hint.height>=64&&hint.width<=capture.width&&
                hint.height<=capture.height&&hint.width<=original.width&&hint.height<=original.height&&
                validatedVideoSrInputExtent(hint,capture,capture)==hint),"policy result satisfies graph validation");
        }
    }
    check(validatedVideoSrInputExtent({1920,1080},{3840,2160},{3840,2160})==Extent{1920,1080},"graph accepts bounded input");
    for(const auto bad:{Extent{},Extent{0,1080},Extent{1920,0},Extent{3841,2160},Extent{1920,1200},Extent{63,36},Extent{3840,2160}})
        check(validatedVideoSrInputExtent(bad,{3840,2160},{3840,2160})==Extent{},"graph rejects malformed or ineffective hint");
    check(validatedVideoSrInputExtent({1920,1080},{3840,2160},{1280,720})==Extent{},"input cannot exceed final working extent");
}
void graphDescription(){
    auto s=settings();auto r=request();pipeline::EnhanceGraphDesc d;
    const auto p=describeStages(r,s,d);
    check(d.videoSrInputExtent==Extent{1920,1080}&&d.enableSr,"SDR realtime RTX VSR accepts hint");
    check(p.source==Extent{2560,1440}&&p.base==Extent{3840,2160}&&p.output==Extent{3840,2160},"VSR projection does not alter resolution plan");
    check(d.sourceWidth==2560&&d.sourceHeight==1440&&d.workWidth==3840&&d.workHeight==2160&&
        d.flowWidth==p.flow.width&&d.flowHeight==p.flow.height,"raw flow and output contracts remain exact");
    check(d.validateFixedExecutionPlan().empty(),"ordinary bounded VSR has valid fixed plan");
    for(uint32_t quality:{0u,kVideoSrFsr}){
        auto other=s;other.videoSrQuality=quality;pipeline::EnhanceGraphDesc next;
        const auto result=describeStages(r,other,next);
        const auto expected=Extent{1920,1080};
        check(next.videoSrInputExtent==expected&&next.enableSr&&result.base==p.base,
            "browser DLSS and FSR accept the explicit source sampling hint");
        other.videoSrInputExtent={};describeStages(r,other,next);
        check(next.videoSrInputExtent==Extent{}&&next.enableSr,
            "local DLSS and FSR without browser metadata preserve full input");
    }
    for(unsigned mode=0;mode<6;++mode){
        auto other=s;auto req=r;pipeline::EnhanceGraphDesc next;
        if(mode==0)req.stillImage=true;
        if(mode==1)req.exportJob=true;
        if(mode==2)next.hdrInput=true;
        if(mode==3)next.hdrOutput=true;
        if(mode==4)req.nvidiaAdapter=false;
        if(mode==5)req.sr=false;
        describeStages(req,other,next);
        check(next.videoSrInputExtent==Extent{},"image export HDR non-NVIDIA or disabled SR ignores projection");
    }
    auto nr=s;nr.nr=true;r.nr=true;describeStages(r,nr,d);
    check(d.videoSrInputExtent==Extent{1920,1080}&&!d.nrBeforeSr&&d.enableNr&&
        d.validateFixedExecutionPlan().empty(),"SR-first NR keeps full NR processing and bounded VSR");
    nr.lowLatency=true;describeStages(r,nr,d);
    check(d.videoSrInputExtent==Extent{}&&d.nrBeforeSr&&d.enableSr,"NR-before-SR uses full source input");
    for(const auto bad:{Extent{0,1080},Extent{3000,2000},Extent{3840,2160},Extent{63,36}}){
        auto other=s;other.videoSrInputExtent=bad;pipeline::EnhanceGraphDesc next;
        describeStages(request(),other,next);
        check(next.videoSrInputExtent==Extent{}&&next.enableSr&&next.validateFixedExecutionPlan().empty(),"invalid projection falls back without dropping normal SR");
    }
    describeStages(request(),s,d);auto image=request();image.stillImage=true;describeStages(image,s,d);
    check(d.videoSrInputExtent==Extent{},"reused description clears previous runtime input");
    const auto frozen=freezeExportSettings(s);
    check(frozen.videoSrInputExtent==Extent{},"export freeze removes runtime browser projection");
}
void identityPlan(){
    auto s=settings();auto r=request({3840,2160});pipeline::EnhanceGraphDesc d;
    const auto p=describeStages(r,s,d);
    check(p.srApplied&&d.enableSr&&p.source==p.base&&p.base==p.output,"4K identity output still evaluates reduced-input VSR");
    check(d.fixedExecutionPlan&&d.fixedExecutionPlan->stepCount==1&&
        d.fixedExecutionPlan->steps[0].type==EffectType::SuperResolution&&
        d.fixedExecutionPlan->steps[0].input==p.source&&d.fixedExecutionPlan->steps[0].output==p.output&&
        d.validateFixedExecutionPlan().empty(),"identity VSR retains one exact SR execution boundary");
    s.nr=true;r.nr=true;describeStages(r,s,d);
    check(d.enableNr&&d.fixedExecutionPlan->stepCount==2&&
        d.fixedExecutionPlan->steps[0].type==EffectType::SuperResolution&&
        d.fixedExecutionPlan->steps[1].type==EffectType::NrEnhance&&
        d.nrLayersExtent[0]==Extent{1920,1080}&&d.validateFixedExecutionPlan().empty(),
        "identity VSR insertion preserves downstream NR extent and order");
    s.videoSrInputExtent={};const auto normal=describeStages(r,s,d);
    check(!normal.srApplied&&!d.enableSr&&d.videoSrInputExtent==Extent{}&&
        d.fixedExecutionPlan->stepCount==1&&d.fixedExecutionPlan->steps[0].type==EffectType::NrEnhance,
        "full-capture switch restores previous same-size SR bypass");
}
void nodeOrder(){
    for(bool nrFirst:{false,true}){
        EffectChain chain;chain.mode=ChainMode::Node;chain.nodeCount=3;
        chain.nodes[0].type=EffectType::Color;chain.nodes[0].enabled=true;chain.nodes[0].color.enabled=true;
        chain.nodes[1].type=nrFirst?EffectType::NrEnhance:EffectType::SuperResolution;chain.nodes[1].enabled=true;
        chain.nodes[2].type=nrFirst?EffectType::SuperResolution:EffectType::NrEnhance;chain.nodes[2].enabled=true;
        auto s=settings();s.nr=true;fromChain(chain,s);auto order=runtimeOrder(chain);
        check(bool(order),"test node order is valid");if(!order)continue;
        auto r=request({3840,2160});r.nr=true;r.nodeOrder=&*order;pipeline::EnhanceGraphDesc d;
        describeStages(r,s,d);
        if(nrFirst){
            check(d.videoSrInputExtent==Extent{}&&!d.enableSr,"node NR-before-SR retains identity full capture");
        }else{
            check(d.videoSrInputExtent==Extent{1920,1080}&&d.enableSr&&d.fixedExecutionPlan->stepCount==3&&
                d.fixedExecutionPlan->steps[0].type==EffectType::Color&&
                d.fixedExecutionPlan->steps[1].type==EffectType::SuperResolution&&
                d.fixedExecutionPlan->steps[2].type==EffectType::NrEnhance,"identity node SR reappears at its original boundary");
        }
        check(d.validateFixedExecutionPlan().empty(),"node projection keeps fixed plan valid");
    }
}
void browserDlssInput(){
    auto s=settings();s.videoSrQuality=0;s.videoSrInputExtent={1280,720};
    auto r=request({3840,2160});pipeline::EnhanceGraphDesc d;
    const auto plan=describeStages(r,s,d);
    check(d.enableSr&&plan.srApplied&&d.videoSrInputExtent==Extent{1280,720},
        "browser 720p source still evaluates DLSS after compositor enlarged it to 4K");
    check(d.sourceWidth==3840&&d.sourceHeight==2160&&d.workWidth==3840&&d.workHeight==2160,
        "browser DLSS keeps raw capture and 4K output extents");
    check(d.flowWidth==1280&&d.flowHeight==720,
        "browser DLSS estimates optical flow at the real SR sampling extent");
    check(d.fixedExecutionPlan&&d.fixedExecutionPlan->stepCount==1&&
        d.fixedExecutionPlan->steps[0].type==EffectType::SuperResolution&&d.validateFixedExecutionPlan().empty(),
        "browser DLSS keeps its real reconstruction step for identity output geometry");
    s.videoSrInputExtent={};const auto legacy=describeStages(r,s,d);
    check(!d.enableSr&&!legacy.srApplied&&d.videoSrInputExtent==Extent{},
        "DLSS without a browser hint preserves the existing identity bypass");
}
void browserFsrInput(){
    auto s=settings();s.videoSrQuality=kVideoSrFsr;s.videoSrInputExtent={1280,720};
    auto r=request({3840,2160});pipeline::EnhanceGraphDesc d;
    const auto plan=describeStages(r,s,d);
    check(d.enableSr&&plan.srApplied&&d.videoSrInputExtent==Extent{1280,720},
        "browser FSR reconstructs the 720p sampling grid even at identity 4K capture geometry");
    check(d.flowWidth==1280&&d.flowHeight==720&&d.srSamplingExtent()==Extent{1280,720},
        "browser FSR color and render-pixel guidance share the original sampling grid");
    check(d.fixedExecutionPlan&&d.fixedExecutionPlan->stepCount==1&&
        d.fixedExecutionPlan->steps[0].type==EffectType::SuperResolution&&d.validateFixedExecutionPlan().empty(),
        "browser FSR retains its reconstruction step in the fixed plan");
    check(d.sourceSizedFsrReset(false,0),"source-sized FSR receives a reset for its initial zero-motion frame");
    check(d.sourceSizedFsrReset(true,90),"source-sized FSR receives every later discontinuity reset even without current flow");
    check(!d.sourceSizedFsrReset(false,90),"ordinary later FSR frames do not qualify for zero-motion initialization");
    s.videoSrInputExtent={};describeStages(r,s,d);
    check(!d.enableSr&&d.videoSrInputExtent==Extent{},"FSR without browser metadata retains identity bypass");
    check(!d.sourceSizedFsrReset(true,90),"no-hint FSR keeps its pre-existing reset path");
}
void browserSourceContracts(){
    for(const auto quality:{0u,kVideoSrFsr}){
        check(browserSrInputExtent({3840,2160},{1280,720},quality,false,true)==Extent{1280,720},
            "DLSS and FSR source policy is independent of the old RTX-only full-capture switches");
        check(browserSrInputExtent({2160,3840},{720,1280},quality,true,false)==Extent{720,1280},
            "portrait original metadata keeps the same browser sampling orientation");
        check(browserSrInputExtent({1920,1080},{3840,2160},quality,true,false)==Extent{},
            "source-sized browser policy never synthesizes a larger SR input");
        check(browserSrInputExtent({3840,2160},{720,1280},quality,true,false)==Extent{},
            "stale aspect metadata cannot reshape temporal SR input");
        for(unsigned mode=0;mode<5;++mode){
            auto s=settings();s.videoSrQuality=quality;s.videoSrInputExtent={1280,720};
            auto r=request({3840,2160});pipeline::EnhanceGraphDesc d;
            if(mode==0)r.stillImage=true;
            if(mode==1)r.exportJob=true;
            if(mode==2)d.hdrInput=true;
            if(mode==3)d.hdrOutput=true;
            if(mode==4)r.sr=false;
            describeStages(r,s,d);
            check(d.videoSrInputExtent==Extent{}&&!d.sourceSizedSr(),
                "export images HDR and disabled SR retain their previous full-input contract");
        }
        auto s=settings();s.videoSrQuality=quality;s.videoSrInputExtent={1280,720};
        auto r=request({3840,2160});pipeline::EnhanceGraphDesc d;
        s.nr=true;s.lowLatency=true;r.nr=true;describeStages(r,s,d);
        check(d.videoSrInputExtent==Extent{}&&!d.sourceSizedSr(),
            "NR-before-SR rejects browser downsampling rather than discarding upstream enhancement");
        s.nr=false;s.lowLatency=false;r.nr=false;describeStages(r,s,d);
        check(d.sourceSizedSr()&&d.srSamplingExtent()==Extent{1280,720},
            "temporal browser SR declares the source-sized resource contract");
        auto wrong=d;wrong.flowWidth=3840;wrong.flowHeight=2160;
        check(!wrong.validateFixedExecutionPlan().empty(),
            "mismatched source-sized motion is rejected before allocating GPU resources");
        const auto frozen=freezeExportSettings(s);
        check(frozen.videoSrInputExtent==Extent{},
            "export freeze strips every browser-only SR sampling hint");
        auto local=s;local.videoSrInputExtent={};
        auto localRequest=request({1920,1080});const auto localPlan=describeStages(localRequest,local,d);
        check(d.enableSr&&!d.sourceSizedSr()&&d.srSamplingExtent()==Extent{1920,1080}&&
            localPlan.output==Extent{3840,2160},
            "ordinary local DLSS and FSR retain their original source-to-4K sampling");
        check(!s.sameVideoConfiguration(local)&&requiresGraphRebuild(s,local),
            "adding or clearing browser sampling forces graph rebuild and temporal history reset");
    }
    for(uint32_t quality=1;quality<=4;++quality){
        check(browserSrInputExtent({3840,2160},{1280,720},quality,true,false)==Extent{1280,720},
            "RTX source-size input remains available at all four quality settings");
        check(browserSrInputExtent({3840,2160},{1280,720},quality,false,false)==Extent{}&&
            browserSrInputExtent({3840,2160},{1280,720},quality,true,true)==Extent{},
            "RTX source-size off and the existing full-capture environment switch preserve full capture");
    }
    check(browserSrInputExtent({3840,2160},{1280,720},6,true,false)==Extent{},
        "unrecognized SR backend never receives an inferred sampling grid");
    auto fsr=settings();fsr.videoSrQuality=kVideoSrFsr;fsr.videoSrInputExtent={1280,720};
    auto r=request({3840,2160});r.nvidiaAdapter=false;pipeline::EnhanceGraphDesc d;describeStages(r,fsr,d);
    check(d.enableSr&&d.sourceSizedSr()&&d.flowWidth==1280&&d.flowHeight==720,
        "browser FSR sampling policy remains available on a vendor-neutral adapter");
    fsr.amdFlowHalfResolution=true;describeStages(r,fsr,d);
    check(d.sourceSizedSr()&&!d.amdFlowHalfResolution&&d.validateFixedExecutionPlan().empty(),
        "source-sized temporal SR avoids a second half-resolution motion reduction");
    fsr.videoSrInputExtent={};describeStages(request({1920,1080}),fsr,d);
    check(!d.sourceSizedSr()&&d.amdFlowHalfResolution,
        "ordinary no-hint FSR retains the saved optical-flow half-resolution setting");
}
void rebuild(){
    auto full=settings();full.videoSrInputExtent={};auto bounded=full;bounded.videoSrInputExtent={1920,1080};
    check(!full.sameVideoConfiguration(bounded)&&requiresGraphRebuild(full,bounded),"input dimension change rebuilds graph");
    auto smaller=bounded;smaller.videoSrInputExtent={1280,720};
    check(!bounded.sameVideoConfiguration(smaller)&&requiresGraphRebuild(bounded,smaller),"second input dimension change rebuilds graph");
    check(!requiresGraphRebuild(bounded,bounded)&&bounded.sameVideoConfiguration(bounded),"unchanged input does not rebuild");
    auto audio=bounded;audio.audioOffsetMs=50;
    check(bounded.sameVideoConfiguration(audio)&&!requiresGraphRebuild(bounded,audio),"unrelated audio live edits remain live");
    auto restored=bounded;fromChain(toChain(restored),restored);
    check(restored.videoSrInputExtent==bounded.videoSrInputExtent,"chain conversion does not discard current runtime projection");
}
}
int main(){
    policy();graphDescription();identityPlan();nodeOrder();browserDlssInput();browserFsrInput();browserSourceContracts();rebuild();
    std::cout<<"BROWSER_RTX_INPUT checks="<<checks<<" failures="<<failures<<'\n';
    return failures?1:0;
}
