// Stage11 full graph synthetic acceptance. No SDK/provider or user-video claim.
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/sink/ImageExportSink.h"
#include "veyra/engine/GraphDescription.h"
#include <d3d12sdklayers.h>
#include <iostream>
#include <memory>
#include <vector>
#include <algorithm>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
using namespace veyra;
struct Frame {
    AVFrame* value=av_frame_alloc();
    ~Frame(){av_frame_free(&value);}
    bool make(bool hdr){
        if(!value)return false;value->width=64;value->height=48;value->format=hdr?AV_PIX_FMT_P010:AV_PIX_FMT_BGR0;
        value->color_range=hdr?AVCOL_RANGE_MPEG:AVCOL_RANGE_UNSPECIFIED;
        value->colorspace=hdr?AVCOL_SPC_BT2020_NCL:AVCOL_SPC_RGB;
        value->color_primaries=hdr?AVCOL_PRI_BT2020:AVCOL_PRI_BT709;
        value->color_trc=hdr?AVCOL_TRC_SMPTE2084:AVCOL_TRC_IEC61966_2_1;
        if(av_frame_get_buffer(value,32)<0)return false;
        for(int y=0;y<48;++y)for(int x=0;x<64;++x){
            if(hdr)reinterpret_cast<uint16_t*>(value->data[0]+size_t(y)*value->linesize[0])[x]=uint16_t((100+x*10)<<6);
            else{auto* p=value->data[0]+size_t(y)*value->linesize[0]+x*4;p[0]=uint8_t(68+(x+y)%7);p[1]=uint8_t(92+(x*3+y)%11);p[2]=uint8_t(148+(x*7+y*3)%13);p[3]=255;}
        }
        if(hdr)for(int y=0;y<24;++y)for(int x=0;x<64;++x)reinterpret_cast<uint16_t*>(value->data[1]+size_t(y)*value->linesize[1])[x]=32768;
        return true;
    }
};
int main(){
    unsigned checks=0,failures=0;auto check=[&](bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<label<<" FAILED\n";}};
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status status=Status::Ok;gfx::DeviceContextDesc dc;dc.enableDebugLayer=true;
    if(!ctx.initialize(dc,status)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),4,status))return 2;
    auto graph=std::make_unique<pipeline::EnhanceGraph>(ctx,ring);
    uint64_t id=0;std::vector<uint8_t> sourceBaseline;
    auto readReference=[&](ID3D12Resource* texture,std::vector<uint8_t>& pixels){
        const auto td=texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows;UINT64 rowBytes,total;
        ctx.device()->GetCopyableFootprints(&td,0,1,0,&fp,&rows,&rowBytes,&total);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=total;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;if(FAILED(ctx.device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))))return false;
        unsigned slot;auto* cmd=ring.acquireNext(slot,status);if(!cmd)return false;
        pipeline::StateTracker states;states.transition(cmd,texture,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=texture;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        b.pResource=readback.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=fp;
        cmd->CopyTextureRegion(&b,0,0,0,&a,nullptr);states.transition(cmd,texture,D3D12_RESOURCE_STATE_COMMON);
        if(!ring.submitAndSignal(slot)||!ring.waitIdle())return false;
        void* data=nullptr;D3D12_RANGE range{0,size_t(total)};if(FAILED(readback->Map(0,&range,&data)))return false;
        pixels.assign(static_cast<uint8_t*>(data),static_cast<uint8_t*>(data)+size_t(total));D3D12_RANGE empty{};readback->Unmap(0,&empty);return true;
    };
    auto start=[&](const engine::EnhancementSettings& settings,bool hdr,bool image=false,bool directImageRequest=false){
        ring.drainQueue();graph->shutdown();
        pipeline::EnhanceGraphDesc desc;desc.noFeatures=true;desc.noNgx=true;desc.rgbInput=!hdr;desc.hdrInput=hdr;desc.stillImage=image;
        engine::StageRequest req;req.width=64;req.height=48;req.nr=false;req.sr=false;req.fg=false;req.stillImage=image;
        engine::describeStages(req,settings,desc);desc.enableNvofStandalone=false;
        if(directImageRequest){desc.beauty=settings.beauty;desc.fixedExecutionPlan.reset();} // direct descriptor must also obey video-only guard
        const bool ready=graph->initialize(desc)&&graph->createViews();
        if(ready)graph->recordGpuTimings(); // same completed-query collection enabled by production engine
        return ready;
    };
    auto run=[&](Frame& frame,std::vector<uint8_t>& pixels,bool expectBeauty,bool paused=false){
        pipeline::EnhanceGraph::FrameOutputs output;
        if(!paused)++id;
        if(!graph->process(frame.value,double(id)*40,id==1,output,id,nullptr,nullptr,true,{},0,paused)||!ring.waitIdle())return false;
        check(output.batch.count==1&&output.batch.frames[0].kind==pipeline::FrameKind::Real,"real frame lease");
        check(output.batch.frames[0].lease&&output.batch.frames[0].lease->ready()&&output.batch.frames[0].lease->referencesValid,"GPU completed lease and references");
        check(output.batch.frames[0].lease->sourceReference!=output.batch.frames[0].lease->texture,"unmodified separate source reference");
        if(frame.value->format==AV_PIX_FMT_BGR0){
            std::vector<uint8_t> reference;if(!readReference(output.batch.frames[0].lease->sourceReference.Get(),reference))return false;
            if(sourceBaseline.empty())sourceBaseline=reference;
            check(reference==sourceBaseline,"source reference pixels retained before beauty");
        }
        sink::RgbaImage image;if(!sink::readRgba8(ctx,ring,graph->videoFrameResource(output.videoSlot),image))return false;
        pixels=image.pixels;const auto timings=graph->takeGpuTimings();
        bool beautyMeasured=false;for(const auto& timing:timings)beautyMeasured|=timing.gpu[size_t(diagnostics::GpuStage::Beauty)].state==diagnostics::SampleState::Measured;
        check(beautyMeasured==expectBeauty,"actual Beauty GPU query only executing frames");
        return true;
    };
    Frame sdr,hdr;if(!sdr.make(false)||!hdr.make(true))return 2;
    engine::EnhancementSettings settings;std::vector<uint8_t> baseline,zero,enhanced,restored;
    check(start(settings,false),"OFF graph starts");if(!run(sdr,baseline,false))return 2;
    check(graph->metrics().beautyEvaluateCount==0&&!graph->beautyActive(),"OFF no evaluation");
    settings.beauty={true,.7f,.4f,.1f};
    for(bool directImageRequest:{false,true}){
        check(start(settings,false,true,directImageRequest),"image graph starts while saved beauty requested ON");
        if(!run(sdr,restored,false))return 2;
        check(restored==baseline&&graph->metrics().beautyEvaluateCount==0&&!graph->beautyActive(),"image beauty bypass primary byte identity");
        check(graph->beautyStatus().find(L"图片")!=std::wstring::npos,"image unsupported explicit status");
        check(settings.beauty.enabled,"image keeps user's saved video beauty request");
    }
    settings.beauty={true,0,0,0};
    check(start(settings,false),"enabled zero graph starts");if(!run(sdr,zero,false))return 2;
    check(zero==baseline,"enabled allzero primary byte identity");
    // Values are live: same graph changes zero -> effect -> zero repeatedly.
    for(unsigned cycle=0;cycle<3;++cycle){
        settings.beauty={true,.7f,.4f,.1f};check(graph->applySettings(settings),"positive live apply");
        if(!run(sdr,enhanced,true))return 2;check(enhanced!=baseline&&graph->beautyActive(),"beauty reaches primary packed output");
        settings.beauty={true,0,0,0};check(graph->applySettings(settings),"zero live apply");
        if(!run(sdr,restored,false))return 2;check(restored==baseline&&!graph->beautyActive(),"live zero no stale enhanced tail");
    }
    check(graph->metrics().beautyEvaluateCount==3,"once per real frame only");
    settings.beauty={true,.7f,.4f,.1f};check(graph->applySettings(settings),"paused effect live apply");
    std::vector<uint8_t> pausedFirst,pausedAgain;
    if(!run(sdr,pausedFirst,true,true)||!run(sdr,pausedAgain,true,true))return 2;
    check(pausedFirst==pausedAgain,"same-ID same-PTS explicit paused refresh no cumulative beauty");
    check(graph->metrics().beautyEvaluateCount==5,"paused explicit refresh executes once per requested process");
    settings.beauty={true,0,0,0};check(graph->applySettings(settings),"paused zero live apply");
    if(!run(sdr,restored,false,true))return 2;
    check(restored==baseline,"paused zero returns source no stale tail");
    settings.beauty={true,.7f,.4f,.1f};
    check(start(settings,true),"HDR beauty graph starts");if(!run(hdr,enhanced,false))return 2;
    check(!graph->beautyActive()&&graph->beautyStatus().find(L"HDR")!=std::wstring::npos&&graph->metrics().beautyEvaluateCount==0,"HDR explicitly bypassed");
    settings.beauty.enabled=false;check(start(settings,true),"HDR baseline starts");if(!run(hdr,baseline,false))return 2;
    check(enhanced==baseline,"HDR primary identity with beauty bypass");
    // Deliberate API refusal, not real GPU memory exhaustion.
    settings.beauty.enabled=true;SetEnvironmentVariableW(L"VEYRA_TEST_BEAUTY_INIT_REFUSE",L"1");
    const bool refused=start(settings,false);SetEnvironmentVariableW(L"VEYRA_TEST_BEAUTY_INIT_REFUSE",nullptr);
    check(refused,"optional beauty init refusal retains graph");
    if(!run(sdr,restored,false))return 2;
    check(graph->metrics().beautyEvaluateCount==0&&!graph->beautyActive()&&graph->beautyStatus().find(L"失败")!=std::wstring::npos,"beauty failure explicit status");
    ring.drainQueue();graph->shutdown();graph.reset();
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> info;if(FAILED(ctx.device()->QueryInterface(IID_PPV_ARGS(&info))))return 2;
    unsigned errors=0;for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* msg=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,msg,&size);if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<msg->pDescription<<"\n";}}
    check(errors==0,"graph D3D12 debug layer clean");std::cout<<"debugErrors="<<errors<<" checks="<<checks<<" failures="<<failures<<"\n";return failures?1:0;
}
