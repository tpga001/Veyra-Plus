#include "veyra/pipeline/BeautyPass.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>
#include <limits>
#include <filesystem>
#include <fstream>
using namespace veyra;
using namespace veyra::pipeline;
namespace veyra::pipeline {
struct BeautyGpuTestAccess {
    static ID3D12Resource* horizontal(const BeautyPass& p){return p.horizontal_.Get();}
    static ID3D12Resource* mean(const BeautyPass& p){return p.mean_.Get();}
};
}
namespace {
#include "BeautyGoldenData.inc"
}
int wmain(int argc,wchar_t** argv){
    std::filesystem::path diagnostics;
    if(argc==3&&std::wstring(argv[1])==L"--diagnostics"){diagnostics=argv[2];std::filesystem::create_directories(diagnostics);}
    else if(argc!=1)return 2;
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status status=Status::Ok;
    gfx::DeviceContextDesc desc;desc.enableDebugLayer=true;
    if(!ctx.initialize(desc,status)||!ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),4,status))return 2;
    constexpr unsigned width=32,height=24,pitch=256,components=width*height*4;
    auto input=makeTexture(ctx.device(),width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
    auto upload=makeUploadBuffer(ctx.device(),pitch*height);
    if(!input||!upload)return 2;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=pitch*height;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if(FAILED(ctx.device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))))return 2;
    StateTracker tracker;void* mapped=nullptr;
    if(FAILED(upload->Map(0,nullptr,&mapped)))return 2;
    auto* half=static_cast<DirectX::PackedVector::HALF*>(mapped);
    for(unsigned i=0;i<components;++i)half[i]=DirectX::PackedVector::XMConvertFloatToHalf(kBeautyInput[i]);
    upload->Unmap(0,nullptr);
    unsigned slot;auto* list=ring.acquireNext(slot,status);if(!list)return 2;
    tracker.transition(list,input.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,width,height,1,pitch};
    dst.pResource=input.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    if(!ring.submitAndSignal(slot)||!ring.waitIdle())return 2;
    BeautyPass pass;if(!pass.initialize(ctx.device(),width,height,input.Get()))return 2;
    unsigned checks=0,failures=0;
    auto check=[&](bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<label<<" FAILED\n";}};
    auto read=[&](ID3D12Resource* texture,std::vector<float>& pixels){
        auto* cmd=ring.acquireNext(slot,status);if(!cmd)return false;
        tracker.transition(cmd,texture,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=texture;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        b.pResource=readback.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,width,height,1,pitch};
        cmd->CopyTextureRegion(&b,0,0,0,&a,nullptr);
        if(!ring.submitAndSignal(slot)||!ring.waitIdle())return false;
        void* data=nullptr;D3D12_RANGE bytes{0,pitch*height};
        if(FAILED(readback->Map(0,&bytes,&data)))return false;
        const auto* values=static_cast<const DirectX::PackedVector::HALF*>(data);pixels.resize(components);
        for(unsigned i=0;i<components;++i)pixels[i]=DirectX::PackedVector::XMConvertHalfToFloat(values[i]);
        D3D12_RANGE empty{};readback->Unmap(0,&empty);return true;
    };
    struct Case{const char* name;engine::BeautySettings settings;const float* expected;};
    const Case cases[]={{"smooth", {true,1,0,0},kBeautySmooth},{"whiten",{true,0,1,0},kBeautyWhiten},
        {"detail independent",{true,0,0,1},kBeautyDetail},{"mixed",{true,.65f,.35f,.1f},kBeautyMixed}};
    std::vector<float> pixels,fullWhitening;
    for(const auto& c:cases){
        for(unsigned repeat=0;repeat<3;++repeat){
            auto* cmd=ring.acquireNext(slot,status);if(!cmd)return 2;
            check(pass.run(cmd,tracker,c.settings),"real dispatch recorded");
            check(ring.submitAndSignal(slot)&&ring.waitIdle(),"real dispatch completed");
            if(!read(pass.result(),pixels))return 2;
            if(c.expected==kBeautyWhiten&&repeat==0)fullWhitening=pixels;
            float error=0;unsigned worst=0;for(unsigned i=0;i<components;++i)if(std::abs(pixels[i]-c.expected[i])>error){error=std::abs(pixels[i]-c.expected[i]);worst=i;}
            std::cout<<c.name<<" repeat="<<repeat<<" maxLinearError="<<error<<" worstComponent="<<worst<<" x="<<(worst/4)%width<<" y="<<(worst/4)/width<<" channel="<<worst%4<<" actual="<<pixels[worst]<<" expected="<<c.expected[worst]<<" input="<<kBeautyInput[worst]<<"\n";
            if(!diagnostics.empty()&&repeat==0){
                auto dump=[&](const std::filesystem::path& name,const std::vector<float>& data){
                    std::ofstream file(diagnostics/name,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()*sizeof(float)));return bool(file);
                };
                std::string name=c.name;std::replace(name.begin(),name.end(),' ','_');
                if(!dump(name+"-output-f32.bin",pixels))return 2;
                if(c.settings.smoothing>0){
                    std::vector<float> horizontal,mean;
                    if(!read(BeautyGpuTestAccess::horizontal(pass),horizontal)||!read(BeautyGpuTestAccess::mean(pass),mean))return 2;
                    if(!dump(name+"-horizontal-f32.bin",horizontal)||!dump(name+"-mean-f32.bin",mean))return 2;
                    for(const auto& item:{std::pair<const char*,const float*>{"horizontal",kBeautyHorizontal}, {"mean",kBeautyMean}}){
                        const auto& actual=item.first==std::string("horizontal")?horizontal:mean;
                        float maximum=0;unsigned index=0;for(unsigned j=0;j<components;++j)if(std::abs(actual[j]-item.second[j])>maximum){maximum=std::abs(actual[j]-item.second[j]);index=j;}
                        std::cout<<"diagnostic "<<item.first<<" maxError="<<maximum<<" worstComponent="<<index<<" x="<<(index/4)%width<<" y="<<(index/4)/width<<" channel="<<index%4<<" actual="<<actual[index]<<" expected="<<item.second[index]<<" outputWorstActual="<<actual[worst]<<" outputWorstExpected="<<item.second[worst]<<"\n";
                    }
                }
            }
            // CPU source RNE and GPU FP16 typed-store RTZ are modeled separately;
            // 8-bit intermediate thresholds remain the GPUPixel equations.
            // no broad visual similarity or full OpenGL DLL byte identity claim.
            check(error<=.0035f,"independent scalar GPUPixel oracle");
            bool alpha=true,finite=true;
            for(unsigned i=0;i<components;++i){finite&=std::isfinite(pixels[i]);if(i%4==3)alpha&=pixels[i]==kBeautyInput[i];}
            check(alpha,"input alpha preserved");check(finite,"finite color output");
        }
    }
    check(pass.evaluations()==12,"one evaluation per real call across slots");
    // Black-box gain contract, independent of individual LUT implementation:
    // tiny gains approach unchanged input and move monotonically toward the
    // observed full endpoint. Float16 quantization may hide tiny pixel steps.
    const auto encode=[](double v){v=std::clamp(v,0.0,1.0);return v<=.0031308?v*12.92:1.055*std::pow(v,1.0/2.4)-.055;};
    const auto halfUlp=[](float v){
        auto h=DirectX::PackedVector::XMConvertFloatToHalf(std::max(0.f,v));
        const auto next=DirectX::PackedVector::HALF(unsigned(h)+1);
        return DirectX::PackedVector::XMConvertHalfToFloat(next)-DirectX::PackedVector::XMConvertHalfToFloat(h);
    };
    const Case lowWhitening[]={{"whiten 0.01 percent",{true,0,.0001f,0},kBeautyWhiten001Percent},
        {"whiten 0.1 percent",{true,0,.001f,0},kBeautyWhiten01Percent},
        {"whiten 1 percent",{true,0,.01f,0},kBeautyWhiten1Percent}};
    if(fullWhitening.size()!=components)return 2;
    std::vector<float> previous(std::begin(kBeautyInput),std::end(kBeautyInput));
    double previousProgress=0;
    for(const auto& c:lowWhitening){
        auto* cmd=ring.acquireNext(slot,status);if(!cmd)return 2;
        check(pass.run(cmd,tracker,c.settings),"low whitening real dispatch");
        check(ring.submitAndSignal(slot)&&ring.waitIdle(),"low whitening completion");
        if(!read(pass.result(),pixels))return 2;
        float oracleError=0,maxDelta=0;bool alpha=true,finite=true,endpointRange=true,monotone=true;
        double numerator=0,denominator=0,quantizationBound=0;
        for(unsigned i=0;i<components;++i){
            oracleError=std::max(oracleError,std::abs(pixels[i]-c.expected[i]));
            finite&=std::isfinite(pixels[i]);
            if(i%4==3){alpha&=pixels[i]==kBeautyInput[i];continue;}
            const float base=kBeautyInput[i],full=fullWhitening[i],current=pixels[i];
            const float ulp=std::max({halfUlp(base),halfUlp(full),halfUlp(current)});
            maxDelta=std::max(maxDelta,std::abs(current-base));
            endpointRange&=current>=std::min(base,full)-2*ulp&&current<=std::max(base,full)+2*ulp;
            const float direction=full-base;
            if(std::abs(direction)>4*ulp){
                if(direction>0)monotone&=current>=previous[i]-2*ulp;
                else monotone&=current<=previous[i]+2*ulp;
            }
            const double origin=encode(base),delta=encode(full)-origin,step=encode(current)-origin;
            numerator+=step*delta;denominator+=delta*delta;
            // Conservative propagation of at most two half-ULP color steps to
            // encoded-domain progress; it is not the broad oracle tolerance.
            const double roundError=std::max(std::abs(encode(current+2*ulp)-encode(current)),
                std::abs(encode(std::max(0.f,current-2*ulp))-encode(current)));
            quantizationBound+=std::abs(delta)*roundError;
        }
        const double progress=denominator>0?numerator/denominator:0;
        const double progressBound=denominator>0?quantizationBound/denominator:0;
        std::cout<<c.name<<" maxLinearError="<<oracleError<<" maxDeltaFromInput="<<maxDelta
            <<" encodedGain="<<progress<<" requestedGain="<<c.settings.whitening<<" halfQuantizationGainBound="<<progressBound<<"\n";
        check(oracleError<=.0035f,"low whitening independent scalar reference");
        check(alpha&&finite,"low whitening finite colors and alpha");
        // sRGB decode derivative is bounded by <2.3 for coded values [0,1].
        // Original discontinuous pregrade fails this tiny-gain continuity bound.
        check(maxDelta<=2.3f*c.settings.whitening+.001f,"low whitening approaches zero without pregrade jump");
        check(endpointRange,"low whitening stays between original and full endpoint");
        check(monotone&&progress+1e-5>=previousProgress,"low whitening progresses monotonically toward full endpoint");
        check(denominator>0&&std::abs(progress-c.settings.whitening)<=progressBound+1e-5,
            "whole-pipeline gain matches observed endpoint within FP16 quantization");
        previous=pixels;previousProgress=progress;
    }
    check(pass.evaluations()==15,"three low-range evaluations recorded once each");
    for(auto no: {engine::BeautySettings{false,1,1,1},engine::BeautySettings{true,0,0,0}}){
        auto* cmd=ring.acquireNext(slot,status);if(!cmd)return 2;
        const auto prior=pass.evaluations();check(!pass.run(cmd,tracker,no),"off or zero records no work");
        check(ring.submitAndSignal(slot)&&ring.waitIdle()&&pass.evaluations()==prior,"no evaluation on bypass");
        if(!read(input.Get(),pixels))return 2;
        check(std::equal(pixels.begin(),pixels.end(),std::begin(kBeautyInput)),"bypass source byte identity");
    }
    engine::BeautySettings invalid{true,std::numeric_limits<float>::quiet_NaN(),0,0};
    list=ring.acquireNext(slot,status);if(!list)return 2;
    check(!pass.run(list,tracker,invalid),"nonfinite rejected without dispatch");
    if(!ring.submitAndSignal(slot)||!ring.waitIdle())return 2;
    ring.drainQueue();ComPtr<ID3D12InfoQueue> info;
    if(SUCCEEDED(ctx.device()->QueryInterface(IID_PPV_ARGS(&info)))){
        unsigned errors=0;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){
            SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);
            auto* msg=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,msg,&size);
            if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<msg->pDescription<<"\n";}
        }
        check(errors==0,"D3D12 debug layer clean");std::cout<<"debugErrors="<<errors<<"\n";
    }else{std::cout<<"debug layer unavailable\n";return 2;}
    pass.close();check(pass.result()==nullptr&&pass.evaluations()==0,"drained close releases scratch");
    std::cout<<"checks="<<checks<<" failures="<<failures<<"\n";return failures?1:0;
}
