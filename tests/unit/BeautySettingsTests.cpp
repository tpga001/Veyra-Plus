#include "veyra/engine/EffectChain.h"
#include "veyra/engine/PresetStore.h"
#include "veyra/engine/PresetLibrary.h"
#include "veyra/engine/GraphDescription.h"
#include "veyra/diagnostics/EnhancementProcessingTime.h"
#include <iostream>
#include <limits>
#include <filesystem>
#include <fstream>
using namespace veyra;
int main(int argc,char** argv){
    if(argc!=2)return 2;
    const auto dir=std::filesystem::path(argv[1]);std::filesystem::create_directories(dir);
    unsigned checks=0,failures=0;
    auto check=[&](bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<label<<" FAILED\n";}};
    engine::EnhancementSettings s;
    check(!s.beauty.enabled&&s.beauty.valid()&&!s.beauty.hasEffect(),"default off");
    for(float v:{-.01f,1.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        auto bad=s;bad.beauty.smoothing=v;check(!bad.validate().empty(),"invalid smoothing rejected");
        bad=s;bad.beauty.whitening=v;check(!bad.validate().empty(),"invalid whitening rejected");
        bad=s;bad.beauty.detail=v;check(!bad.validate().empty(),"invalid detail rejected");
    }
    auto live=s;live.beauty.smoothing=.7f;live.beauty.whitening=.3f;live.beauty.detail=.1f;
    check(s.sameVideoConfiguration(live)&&!engine::requiresGraphRebuild(s,live),"strength live not topology");
    live.beauty.enabled=true;
    check(!s.sameVideoConfiguration(live)&&engine::requiresGraphRebuild(s,live),"enable topology");
    auto chain=engine::toChain(live);check(chain.countOf(engine::EffectType::Beauty)==1,"beauty singleton");
    check(engine::effectTypeName(engine::EffectType::Beauty)=="beauty"&&int(engine::EffectType::Beauty)==6,"stable type IDs");
    engine::EnhancementSettings restored;engine::fromChain(chain,restored);
    check(restored.beauty==live.beauty,"chain payload roundtrip");
    auto wrong=chain;wrong.nodes[0]=*chain.firstOf(engine::EffectType::Beauty);wrong.nodes[1].enabled=true;
    wrong.nodes[chain.nodeCount-3].enabled=false; // duplicated singleton still must fail
    check(!engine::validateChain(wrong).accepted,"invalid order/singleton rejected");
    engine::EffectChain order;order.nodeCount=2;order.nodes[0].type=engine::EffectType::Beauty;order.nodes[0].enabled=true;
    order.nodes[1].type=engine::EffectType::SuperResolution;order.nodes[1].enabled=true;
    check(!engine::validateChain(order).accepted,"beauty before SR rejected");
    std::swap(order.nodes[0],order.nodes[1]);check(engine::validateChain(order).accepted,"SR before beauty accepted");
    check(!live.beauty.supported(true,false)&&!live.beauty.supported(false,true)&&live.beauty.supported(false,false),"HDR policy");
    pipeline::EnhanceGraphDesc desc;engine::StageRequest request;request.width=1920;request.height=1080;request.nr=false;request.sr=false;request.fg=false;
    engine::describeStages(request,live,desc);
    check(desc.beauty==live.beauty&&desc.fixedExecutionPlan&&desc.validateFixedExecutionPlan().empty(),"graph propagation fixed plan");
    request.stillImage=true;engine::describeStages(request,live,desc);
    check(desc.stillImage&&!desc.beauty.enabled&&desc.fixedExecutionPlan&&desc.fixedExecutionPlan->resourceCounts[size_t(engine::EffectType::Beauty)]==0,"image plan has zero beauty resources");
    check(live.beauty.enabled&&live.beauty.smoothing==.7f,"image normalization preserves saved video beauty");
    // Complete presets preserve values; old schema remains disabled.
    engine::PresetStore store(dir/"beauty-presets.txt");
    check(store.put(L"Beauty",live)&&store.setDefault(0),"preset write");
    engine::PresetStore read(dir/"beauty-presets.txt");check(read.load()&&read.defaultSettings().beauty==live.beauty,"preset31 roundtrip");
    engine::PresetStore old(dir/"legacy-presets.txt");check(old.put(L"Old",s)&&old.setDefault(0),"old default schema save");
    engine::PresetStore oldRead(dir/"legacy-presets.txt");check(oldRead.load()&&!oldRead.defaultSettings().beauty.enabled,"old schema off migration");
    auto session=engine::ChainSession::initial(live);engine::ChainSessionStore ss(dir/"beauty-session.txt");
    check(ss.save(session),"session7 save");auto loaded=engine::ChainSession::initial({});
    check(ss.load(loaded)&&loaded==session,"session beauty roundtrip");
    // Node enabled is runtime authority, but dormant payload flags must roundtrip
    // exactly (editor serialization temporarily disables every node envelope).
    for(bool nodeEnabled:{false,true}){
        auto varied=session;auto* beauty=varied.configurations[0].chain.firstOf(engine::EffectType::Beauty);
        beauty->enabled=nodeEnabled;beauty->beauty.enabled=!nodeEnabled;
        engine::ChainSessionStore variedStore(dir/(nodeEnabled?"beauty-node-on.txt":"beauty-node-off.txt"));
        auto variedRead=engine::ChainSession::initial({});
        check(variedStore.save(varied)&&variedStore.load(variedRead)&&variedRead==varied,"node flag and inner payload preserved independently");
    }
    diagnostics::GpuFrameTiming timing;
    auto measured=[&](diagnostics::GpuStage stage,uint64_t begin,uint64_t end){auto& v=timing.gpu[size_t(stage)];v.state=diagnostics::SampleState::Measured;v.begin=begin;v.end=end;v.frequency=1000000;v.milliseconds=double(end-begin)/1000;};
    measured(diagnostics::GpuStage::Color,100,200);measured(diagnostics::GpuStage::Nr,200,400);measured(diagnostics::GpuStage::NrLayer0,200,400);
    measured(diagnostics::GpuStage::Beauty,400,700);measured(diagnostics::GpuStage::Blit,900,2000);
    check(diagnostics::graphExecutionSpanMs(timing)==.6,"beauty in graph envelope excludes Blit");
    check(diagnostics::enhancementProcessingMs(timing)==.5,"beauty union span no NR doublecount");
    std::cout<<"checks="<<checks<<" failures="<<failures<<"\n";return failures?1:0;
}
