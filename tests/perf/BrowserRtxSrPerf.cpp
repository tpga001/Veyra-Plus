// Product-graph VSR4 diagnostic. Synthetic BGRA GPU capture, no browser/scanout.
// --input 3518x1975 --output 3840x2154 [--vsr-input 1920x1076]
// --frames 300 --warmup 60 --out E:/.../new.json
// Separate correctness run: --readback --frames 12 (serialized PNG/hash capture).
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "veyra/FileIdentity.h"
#include "veyra/RuntimePaths.h"
#include "veyra/diagnostics/GpuTimer.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/pipeline/FramePacket.h"
#include "veyra/sink/ImageExportSink.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
using namespace veyra;
using Microsoft::WRL::ComPtr;
namespace {
using Clock=std::chrono::steady_clock;
using Extent=pipeline::Extent;
constexpr unsigned baseWidth=1920,baseHeight=1080;
std::string utf8(const std::wstring& value){
    if(value.empty())return {};
    const int n=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    std::string out(size_t(n),'\0');
    if(n)WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),out.data(),n,nullptr,nullptr);
    return out;
}
std::string quote(const std::string& value){
    std::ostringstream out;out<<'"';
    for(unsigned char c:value){switch(c){case '"':out<<"\\\"";break;case '\\':out<<"\\\\";break;
        case '\n':out<<"\\n";break;case '\r':out<<"\\r";break;case '\t':out<<"\\t";break;
        default:if(c<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;else out<<char(c);}}
    out<<'"';return out.str();
}
void require(bool ok,const char* operation){if(!ok)throw std::runtime_error(operation);}
void hr(HRESULT value,const char* operation){if(FAILED(value)){std::ostringstream out;out<<operation<<" HRESULT=0x"<<std::hex<<uint32_t(value);throw std::runtime_error(out.str());}}
Extent extent(const std::wstring& value){
    const auto x=value.find_first_of(L"xX");require(x!=std::wstring::npos,"extent must be WxH");
    size_t a=0,b=0;const auto w=std::stoul(value.substr(0,x),&a),h=std::stoul(value.substr(x+1),&b);
    require(a==x&&b==value.size()-x-1&&w>=16&&h>=16&&w<=8192&&h<=8192&&uint64_t(w)*h<=36000000,"extent outside bounded diagnostic range");
    return {uint32_t(w),uint32_t(h)};
}
unsigned number(const std::wstring& value,unsigned maximum){size_t count=0;const auto n=std::stoul(value,&count);require(count==value.size()&&n<=maximum,"invalid bounded frame count");return unsigned(n);}
struct Options{Extent input{3518,1975},output{3840,2154},vsrInput{};unsigned frames=300,warmup=60;bool readback=false;std::filesystem::path out;};
Options options(int argc,wchar_t** argv){
    Options o;
    for(int i=1;i<argc;++i){const std::wstring a=argv[i];auto next=[&](){require(i+1<argc,"missing option value");return std::wstring(argv[++i]);};
        if(a==L"--input")o.input=extent(next());else if(a==L"--output")o.output=extent(next());else if(a==L"--vsr-input")o.vsrInput=extent(next());
        else if(a==L"--frames")o.frames=number(next(),10000);else if(a==L"--warmup")o.warmup=number(next(),1000);
        else if(a==L"--out")o.out=next();else if(a==L"--readback")o.readback=true;else throw std::runtime_error("unknown option: "+utf8(a));}
    require(o.frames>0&&!o.out.empty()&&o.out.is_absolute(),"--out must be an absolute new JSON path and frames must be positive");
    require(!std::filesystem::exists(o.out),"refusing to replace an existing result");
    require(!o.readback||o.frames<=120,"--readback is limited to 120 frames; use --frames 12 for a pixel run");
    const auto v=o.vsrInput==Extent{}?o.input:o.vsrInput;
    require(v!=o.output,"VSR input and output must differ; bypass is not a valid VSR performance test");
    require(v.width<=o.input.width&&v.height<=o.input.height,"VSR input hint may only reduce the capture extent");
    std::filesystem::create_directories(o.out.parent_path());return o;
}
// The timeout covers provider calls and teardown, not just the frame loop.
struct Budget{
    std::mutex mutex;std::condition_variable cv;bool done=false;std::thread worker;
    explicit Budget(std::filesystem::path out):worker([this,out]{std::unique_lock lock(mutex);
        if(!cv.wait_for(lock,std::chrono::seconds(290),[this]{return done;})){
            std::ofstream f(out.wstring()+L".timeout.json",std::ios::binary);f<<"{\"passed\":false,\"reason\":\"290 second watchdog expired\"}\n";f.close();
            std::cerr<<"BROWSER_RTX_SR_TIMEOUT\n"<<std::flush;TerminateProcess(GetCurrentProcess(),124);}}){}
    ~Budget(){{std::lock_guard lock(mutex);done=true;}cv.notify_all();worker.join();}
};
D3D12_RESOURCE_DESC textureDesc(unsigned w,unsigned h,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
    d.DepthOrArraySize=d.MipLevels=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.Flags=flags;return d;
}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
struct CapturePattern{
    ComPtr<ID3D12Resource> base,capture,upload;ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;ComPtr<ID3D12DescriptorHeap> srv,rtv;
    diagnostics::GpuTimer timer;sink::RgbaImage authored;
    void initialize(gfx::D3D12DeviceContext& ctx,gfx::CommandSlotRing& ring,Extent captureExtent){
        auto* device=ctx.device();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        auto d=textureDesc(baseWidth,baseHeight);
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&base)),"create authored source texture");
        d=textureDesc(captureExtent.width,captureExtent.height,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&capture)),"create BGRA capture texture");
        auto baseDesc=base->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;
        device->GetCopyableFootprints(&baseDesc,0,1,0,&fp,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=1;
        buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"create one-time authored upload");
        authored.width=baseWidth;authored.height=baseHeight;authored.pixels.resize(size_t(baseWidth)*baseHeight*4);
        void* mapped=nullptr;D3D12_RANGE empty{0,0};hr(upload->Map(0,&empty,&mapped),"map authored upload");
        for(unsigned y=0;y<baseHeight;++y)for(unsigned x=0;x<baseWidth;++x){
            // Fine lines, gradients, saturated patches and near-gray detail.
            // Authored once: every measured input frame is GPU-only.
            const unsigned checker=((x/11)^(y/11))&1;const bool line=(x%113<2)||(y%79<2);
            const uint8_t r=uint8_t(line?235:32+(x*173/baseWidth+checker*29)%192);
            const uint8_t g=uint8_t(line?220:24+(y*181/baseHeight+(x/91)*7)%208);
            const uint8_t b=uint8_t(line?210:20+((x+y)*139/(baseWidth+baseHeight)+checker*37)%216);
            auto* bgra=static_cast<uint8_t*>(mapped)+fp.Offset+size_t(y)*fp.Footprint.RowPitch+x*4;
            bgra[0]=b;bgra[1]=g;bgra[2]=r;bgra[3]=255;
            auto* rgba=authored.pixels.data()+(size_t(y)*baseWidth+x)*4;rgba[0]=r;rgba[1]=g;rgba[2]=b;rgba[3]=255;
        }
        upload->Unmap(0,nullptr);Status status=Status::Ok;unsigned slot=0;auto* list=ring.acquireNext(slot,status);require(list!=nullptr,"acquire initialization command slot");
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;
        to.pResource=base.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        transition(list,base.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        require(ring.submitAndSignal(slot)&&ring.waitIdle(),"complete one-time authored upload");upload.Reset();
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=1;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srv)),"create authored SRV heap");
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"create capture RTV heap");
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sd.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(base.Get(),&sd,srv->GetCPUDescriptorHandleForHeapStart());
        device->CreateRenderTargetView(capture.Get(),nullptr,rtv->GetCPUDescriptorHandleForHeapStart());
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;range.OffsetInDescriptorsFromTableStart=0;
        D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants={0,0,1};parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
        sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=parameters;rd.NumStaticSamplers=1;rd.pStaticSamplers=&sampler;
        rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized,error;
        hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error),"serialize capture root signature");
        hr(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)),"create capture root signature");
        const char shader[]=R"HLSL(
Texture2D<float4> sourceTexture:register(t0);SamplerState sourceSampler:register(s0);
cbuffer Constants:register(b0){uint frameIndex;}
struct V{float4 position:SV_Position;float2 uv:TEXCOORD0;};
V vs(uint id:SV_VertexID){V v;v.uv=float2((id<<1)&2,id&2);v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float4 ps(V input):SV_Target{return sourceTexture.SampleLevel(sourceSampler,input.uv+float2(frameIndex%61,frameIndex%37)/float2(1920,1080),0);}
)HLSL";
        HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);require(compiler!=nullptr,"load system shader compiler");
        struct Module{HMODULE value;~Module(){FreeLibrary(value);}} module{compiler};
        const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile"));require(compile!=nullptr,"resolve system shader compiler");
        ComPtr<ID3DBlob> vs,ps;auto compileOne=[&](const char* entry,const char* profile,ComPtr<ID3DBlob>& code){
            ComPtr<ID3DBlob> errors;const HRESULT result=compile(shader,sizeof(shader)-1,"BrowserRtxSrPerf.embedded",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
            if(FAILED(result)&&errors)std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),std::streamsize(errors->GetBufferSize()));hr(result,"compile capture renderer");};
        compileOne("vs","vs_5_0",vs);compileOne("ps","ps_5_0",ps);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        auto& blend=pd.BlendState.RenderTarget[0];blend.SrcBlend=D3D12_BLEND_ONE;blend.DestBlend=D3D12_BLEND_ZERO;blend.BlendOp=D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pd.SampleMask=UINT_MAX;
        pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
        pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.StencilEnable=FALSE;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pd.DepthStencilState.StencilReadMask=D3D12_DEFAULT_STENCIL_READ_MASK;pd.DepthStencilState.StencilWriteMask=D3D12_DEFAULT_STENCIL_WRITE_MASK;
        pd.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};pd.DepthStencilState.BackFace=pd.DepthStencilState.FrontFace;
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_B8G8R8A8_UNORM;pd.SampleDesc.Count=1;
        hr(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)),"create capture renderer pipeline");
        require(timer.initialize(device,ring.queue()),"initialize capture GPU timestamps");timer.recordCompleted();
    }
    uint64_t render(gfx::D3D12DeviceContext& ctx,gfx::CommandSlotRing& ring,Extent size,uint64_t id){
        Status status=Status::Ok;unsigned slot=0;auto* list=ring.acquireNext(slot,status);require(list!=nullptr,"acquire capture command slot");
        timer.frame({1,1,id},ctx.fence());timer.mark(list,diagnostics::GpuStage::Color);
        transition(list,capture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->SetPipelineState(pso.Get());list->SetGraphicsRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]{srv.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetGraphicsRoot32BitConstant(0,unsigned(id-1),0);list->SetGraphicsRootDescriptorTable(1,srv->GetGPUDescriptorHandleForHeapStart());
        D3D12_VIEWPORT viewport{0,0,float(size.width),float(size.height),0,1};D3D12_RECT scissor{0,0,LONG(size.width),LONG(size.height)};
        list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);const auto target=rtv->GetCPUDescriptorHandleForHeapStart();list->OMSetRenderTargets(1,&target,FALSE,nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
        transition(list,capture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
        timer.mark(list,diagnostics::GpuStage::Color,true);timer.resolve(list);require(ring.submitAndSignal(slot),"submit capture renderer");timer.submitted(ring.lastSignaledValue());return ring.lastSignaledValue();
    }
};
// Explicit diagnostics only. The product sink rejects BGRA8 as RGBA8, so
// swizzle once here and then use its existing WIC PNG writer.
sink::RgbaImage readCapture(gfx::D3D12DeviceContext& ctx,gfx::CommandSlotRing& ring,ID3D12Resource* texture){
    const auto d=texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;ctx.device()->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> rb;
    hr(ctx.device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb)),"create diagnostic capture readback");
    Status status=Status::Ok;unsigned slot=0;auto* list=ring.acquireNext(slot,status);require(list!=nullptr,"acquire diagnostic readback slot");
    transition(list,texture,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=rb.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    transition(list,texture,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    require(ring.submitAndSignal(slot)&&ring.waitIdle(),"complete explicit capture readback");
    D3D12_RANGE range{0,SIZE_T(bytes)};void* data=nullptr;hr(rb->Map(0,&range,&data),"map diagnostic capture pixels");
    sink::RgbaImage image;image.width=unsigned(d.Width);image.height=d.Height;image.pixels.resize(size_t(image.width)*image.height*4);
    for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x){const auto* src=static_cast<const uint8_t*>(data)+fp.Offset+size_t(y)*fp.Footprint.RowPitch+x*4;
        auto* dst=image.pixels.data()+(size_t(y)*image.width+x)*4;dst[0]=src[2];dst[1]=src[1];dst[2]=src[0];dst[3]=src[3];}
    D3D12_RANGE empty{0,0};rb->Unmap(0,&empty);return image;
}
struct Sample{uint64_t id=0;double sr=-1,color=-1,graph=-1,inputRender=-1;};
struct Pixels{uint64_t id=0;std::string captureHash,outputHash,capturePng,outputPng;};
struct Result{
    bool passed=false,deviceAlive=false,debugAvailable=false;uint32_t removedReason=0;
    uint64_t processed=0,vsrEvaluations=0,nrEvaluations=0,fgGenerated=0,beautyEvaluations=0,slotWaits=0,commandSubmits=0;
    uint64_t vramBudget=0,vramUsage=0;double slotWaitMs=0,finalDrainMs=0,elapsedMs=0;
    std::string error,authoredHash;gfx::AdapterInfo adapter;Extent actualVsrInput{},actualOutput{};
    std::vector<Sample> samples;std::vector<double> cpuSubmit,cpuInput,cpuSlotWait;std::vector<Pixels> pixels;
};
void aggregate(std::ostream& out,std::vector<double> values){
    values.erase(std::remove_if(values.begin(),values.end(),[](double v){return !std::isfinite(v)||v<0;}),values.end());std::sort(values.begin(),values.end());
    out<<"{\"samples\":"<<values.size();
    if(values.empty())out<<",\"p50Ms\":null,\"p95Ms\":null,\"p99Ms\":null,\"meanMs\":null,\"maxMs\":null}";
    else{auto p=[&](double f){return values[std::min(values.size()-1,size_t(std::ceil(values.size()*f))-1)];};double sum=0;for(double v:values)sum+=v;
        out<<",\"p50Ms\":"<<p(.5)<<",\"p95Ms\":"<<p(.95)<<",\"p99Ms\":"<<p(.99)<<",\"meanMs\":"<<sum/values.size()<<",\"maxMs\":"<<values.back()<<'}';}
}
std::string extentJson(Extent e){return "{\"width\":"+std::to_string(e.width)+",\"height\":"+std::to_string(e.height)+"}";}
void writeResult(const Options& o,const Result& r){
    std::ostringstream j;j<<std::fixed<<std::setprecision(6);
    j<<"{\"schema\":1,\"probe\":\"BrowserRtxSrPerf\",\"passed\":"<<(r.passed?"true":"false")<<",\"error\":"<<quote(r.error)
        <<",\"scope\":\"synthetic GPU BGRA capture simulation; no WGC, browser decoder, presentation or displayed-FPS proof\""
        <<",\"mode\":{\"captureResampled\":true,\"originalDecodedVideo\":false,\"readback\":"<<(o.readback?"true":"false")
        <<",\"performanceComparable\":"<<(o.readback?"false":"true")<<",\"vsrQuality\":4,\"nr\":false,\"fg\":false,\"beauty\":false,\"hdr\":false}"
        <<",\"authoredSource\":"<<extentJson({baseWidth,baseHeight})<<",\"captureExtent\":"<<extentJson(o.input)
        <<",\"requestedVsrInputExtent\":"<<extentJson(o.vsrInput)<<",\"selectedVsrInputExtent\":"<<extentJson(r.actualVsrInput)
        <<",\"selectedOutputExtent\":"<<extentJson(r.actualOutput)<<",\"authoredRgbaSha256\":"<<quote(r.authoredHash)
        <<",\"warmupFrames\":"<<o.warmup<<",\"requestedMeasuredFrames\":"<<o.frames<<",\"processedIncludingWarmup\":"<<r.processed
        <<",\"vsrEvaluations\":"<<r.vsrEvaluations<<",\"nrEvaluations\":"<<r.nrEvaluations<<",\"fgGenerated\":"<<r.fgGenerated<<",\"beautyEvaluations\":"<<r.beautyEvaluations
        <<",\"adapter\":{\"description\":"<<quote(utf8(r.adapter.description))<<",\"vendorId\":"<<r.adapter.vendorId<<",\"deviceId\":"<<r.adapter.deviceId
        <<",\"luid\":"<<quote(r.adapter.luidString)<<",\"driver\":"<<quote(utf8(r.adapter.driverVersion))<<",\"dedicatedVideoMemoryBytes\":"<<r.adapter.dedicatedVideoMemoryBytes<<'}'
        <<",\"gpuDevice\":{\"alive\":"<<(r.deviceAlive?"true":"false")<<",\"removedReason\":"<<r.removedReason
        <<",\"debugLayerEnabled\":"<<(r.debugAvailable?"true":"false")<<",\"debugErrorMessages\":null,\"debugErrorsNotMeasured\":true}"
        <<",\"vramBudgetBytes\":"<<r.vramBudget<<",\"vramUsageBytes\":"<<r.vramUsage<<",\"elapsedMs\":"<<r.elapsedMs
        <<",\"finalDrainCpuMs\":"<<r.finalDrainMs<<",\"steadySlotReuseWaitCount\":"<<r.slotWaits<<",\"steadySlotReuseWaitTotalMs\":"<<r.slotWaitMs
        <<",\"steadyCommandSubmits\":"<<r.commandSubmits<<",\"srSpanIncludes\":\"RGBA8 encode, VSR evaluate, decode, and any configured capture-to-VSR GPU resampling\""
        <<",\"gpu\":{";
    for(unsigned stage=0;stage<4;++stage){if(stage)j<<',';j<<quote(stage==0?"sr":stage==1?"color":stage==2?"graph":"inputRender")<<':';std::vector<double> v;
        for(const auto& s:r.samples)v.push_back(stage==0?s.sr:stage==1?s.color:stage==2?s.graph:s.inputRender);aggregate(j,std::move(v));}
    j<<"},\"cpuSubmit\":";aggregate(j,r.cpuSubmit);j<<",\"cpuInputRenderSubmit\":";aggregate(j,r.cpuInput);j<<",\"cpuSlotReuseWaitPerFrame\":";aggregate(j,r.cpuSlotWait);
    j<<",\"samples\":[";for(size_t i=0;i<r.samples.size();++i){if(i)j<<',';const auto& s=r.samples[i];j<<"{\"sourceFrameId\":"<<s.id<<",\"srMs\":"<<s.sr<<",\"colorMs\":"<<s.color<<",\"graphMs\":"<<s.graph<<",\"inputRenderMs\":"<<s.inputRender<<'}';}
    j<<"],\"pixelFrames\":[";for(size_t i=0;i<r.pixels.size();++i){if(i)j<<',';const auto& p=r.pixels[i];j<<"{\"sourceFrameId\":"<<p.id<<",\"captureSha256\":"<<quote(p.captureHash)<<",\"outputSha256\":"<<quote(p.outputHash)<<",\"capturePng\":"<<quote(p.capturePng)<<",\"outputPng\":"<<quote(p.outputPng)<<'}';}
    j<<"]}\n";std::ofstream output(o.out,std::ios::binary|std::ios::out);require(bool(output),"open result JSON");output<<j.str();output.close();require(bool(output),"write result JSON");std::cout<<j.str();
}
struct Fixture{
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;CapturePattern pattern;
    std::unique_ptr<pipeline::EnhanceGraph> graph;AVFrame* frame=nullptr;bool closed=false;
    void close(){if(closed)return;if(ring.initialized())ring.drainQueue();if(graph)graph->shutdown();graph.reset();av_frame_free(&frame);pattern.timer.close();ring.shutdown();ctx.shutdown();closed=true;}
    ~Fixture(){close();}
};
} // namespace
int wmain(int argc,wchar_t** argv){
    Options o;try{o=options(argc,argv);}catch(const std::exception& e){std::cerr<<e.what()<<"\nUsage: --input WxH --output WxH [--vsr-input WxH] --frames N --warmup N --out <new absolute JSON> [--readback]\n";return 2;}
    Budget budget(o.out);const auto started=Clock::now();Result result;const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);const bool ownCom=SUCCEEDED(com);
    {
        Fixture f;
        try{
            require(ownCom,"initialize COM");SetEnvironmentVariableW(L"VEYRA_VERBOSE_FRAME_LOGS",L"0");
            require(!GetEnvironmentVariableW(L"VEYRA_GRAPH_OFF",nullptr,0)&&!GetEnvironmentVariableW(L"VEYRA_NO_FEATURES",nullptr,0)&&!GetEnvironmentVariableW(L"VEYRA_NO_NGX",nullptr,0),"graph/provider bypass environment is forbidden");
            Status status=Status::Ok;gfx::DeviceContextDesc dd;dd.requiredVendorId=0x10DE;dd.commandSlotCount=4;dd.enableDebugLayer=false;
            require(f.ctx.initialize(dd,status),"initialize NVIDIA D3D12 device");result.adapter=f.ctx.adapter();result.debugAvailable=f.ctx.debugLayerEnabled();
            require(result.adapter.isNvidia&&!result.adapter.isSoftware,"selected adapter must be NVIDIA hardware");
            require(f.ring.initialize(f.ctx.device(),f.ctx.directQueue(),f.ctx.fence(),f.ctx.fenceEvent(),4,status),"initialize four-slot product ring");
            f.pattern.initialize(f.ctx,f.ring,o.input);result.authoredHash=sha256Hex(f.pattern.authored.pixels.data(),f.pattern.authored.pixels.size());
            f.graph=std::make_unique<pipeline::EnhanceGraph>(f.ctx,f.ring);pipeline::EnhanceGraphDesc d;
            d.sourceWidth=o.input.width;d.sourceHeight=o.input.height;d.workWidth=o.output.width;d.workHeight=o.output.height;
            d.videoSrInputExtent=o.vsrInput;d.videoSrQuality=4;d.rgbInput=true;d.enableSr=true;d.enableNr=d.enableFg=d.enableNvofStandalone=false;
            d.beauty.enabled=d.videoHdr.enabled=d.hdrInput=d.hdrOutput=d.color.enabled=false;d.nrHoldStrength=0;d.settingsRevision=1;
            d.runtimeAbsPath=runtime::localRuntimeDirectory().wstring();
            require(f.graph->initialize(d)&&f.graph->createViews()&&f.graph->srEnabled(),"initialize actual VSR4 product graph");f.graph->recordGpuTimings();
            auto* selected=f.graph->diagnosticVideoSrInput();require(selected!=nullptr,"VSR input resource must exist");const auto selectedDesc=selected->GetDesc();
            result.actualVsrInput={unsigned(selectedDesc.Width),selectedDesc.Height};result.actualOutput={f.graph->workWidth(),f.graph->workHeight()};
            require(result.actualVsrInput==(o.vsrInput==Extent{}?o.input:o.vsrInput)&&result.actualOutput==o.output,"actual VSR input/output differ from requested dimensions");
            f.frame=av_frame_alloc();require(f.frame!=nullptr,"allocate hardware frame metadata");f.frame->format=AV_PIX_FMT_D3D11;f.frame->width=int(o.input.width);f.frame->height=int(o.input.height);
            // The graph retains hardware ingress with av_frame_clone. Without
            // a refcounted AVBufferRef, av_frame_ref tries to allocate/copy an
            // opaque D3D11 frame and fails before any graph GPU command records.
            // The actual texture is owned by this fixture until the final drain;
            // this byte is metadata ownership only, not CPU video pixel storage.
            f.frame->buf[0]=av_buffer_alloc(1);require(f.frame->buf[0]!=nullptr,"allocate refcounted hardware metadata token");
            f.frame->data[0]=f.frame->buf[0]->data;
            AVFrame* retainedProbe=av_frame_clone(f.frame);require(retainedProbe!=nullptr,"hardware input metadata must support av_frame_clone");av_frame_free(&retainedProbe);
            f.frame->color_range=AVCOL_RANGE_JPEG;f.frame->color_primaries=AVCOL_PRI_BT709;f.frame->color_trc=AVCOL_TRC_IEC61966_2_1;f.frame->colorspace=AVCOL_SPC_RGB;
            std::map<uint64_t,Sample> samples;auto collect=[&]{
                for(const auto& t:f.graph->takeGpuTimings())if(t.identity.sourceFrameId>o.warmup){auto& s=samples[t.identity.sourceFrameId];s.id=t.identity.sourceFrameId;
                    const auto& sr=t.gpu[size_t(diagnostics::GpuStage::Sr)];const auto& color=t.gpu[size_t(diagnostics::GpuStage::Color)];
                    s.sr=sr.milliseconds.value_or(-1);s.color=color.milliseconds.value_or(-1);s.graph=diagnostics::graphExecutionSpanMs(t).value_or(-1);}
                f.pattern.timer.collect(f.ctx.fence());for(const auto& t:f.pattern.timer.takeCompleted())if(t.identity.sourceFrameId>o.warmup){auto& s=samples[t.identity.sourceFrameId];s.id=t.identity.sourceFrameId;s.inputRender=t.gpu[size_t(diagnostics::GpuStage::Color)].milliseconds.value_or(-1);}
            };
            std::filesystem::path pixelDir=o.out.parent_path()/(o.out.stem().wstring()+L"-pixels");
            if(o.readback){require(!std::filesystem::exists(pixelDir),"refusing to reuse existing pixel evidence directory");std::filesystem::create_directories(pixelDir);require(sink::saveImage((pixelDir/L"authored-1920x1080.png").wstring(),f.pattern.authored),"save authored reference PNG");}
            uint64_t startWaits=f.ring.cpuWaitCount(),startSubmits=f.ring.submitCount();double startWaitMs=f.ring.cpuWaitMilliseconds();
            for(unsigned n=0;n<o.warmup+o.frames;++n){
                if(n==o.warmup){require(f.ring.waitIdle(),"drain warmup boundary");collect();startWaits=f.ring.cpuWaitCount();startWaitMs=f.ring.cpuWaitMilliseconds();startSubmits=f.ring.submitCount();}
                pipeline::FramePacket p;p.sequence=uint64_t(n)+1;p.pts={int64_t(n),30,true};p.duration={1,30,true};p.sourceKind=pipeline::SourceKind::ScreenCapture;
                p.colorInfo.pixelFormat=pipeline::SourcePixelFormat::Bgra8;p.colorInfo.range=pipeline::ColorRange::Full;p.colorInfo.transfer=pipeline::TransferFunction::SRGB;
                p.colorInfo.primaries=pipeline::ColorPrimaries::BT709;p.colorInfo.matrix=pipeline::YuvMatrix::BT709;p.colorInfo.preserveSdrCodeValues=true;
                const double beforeWait=f.ring.cpuWaitMilliseconds();const auto inputStart=Clock::now();const auto captureFence=f.pattern.render(f.ctx,f.ring,o.input,p.sequence);
                const double inputMs=std::chrono::duration<double,std::milli>(Clock::now()-inputStart).count();
                p.hardwareSurface={f.pattern.capture.Get(),0,f.ctx.fence(),captureFence,o.input.width,o.input.height};
                pipeline::EnhanceGraph::FrameOutputs out;const auto submitStart=Clock::now();
                require(f.graph->process(f.frame,p.pts.toDouble()*1000,n==0,out,p.sequence,&p.colorInfo,&p.hardwareSurface,false),"VSR4 process failed");
                const double submitMs=std::chrono::duration<double,std::milli>(Clock::now()-submitStart).count();++result.processed;
                if(n>=o.warmup){result.cpuSubmit.push_back(submitMs);result.cpuInput.push_back(inputMs);result.cpuSlotWait.push_back(f.ring.cpuWaitMilliseconds()-beforeWait);
                    if(o.readback){auto capture=readCapture(f.ctx,f.ring,f.pattern.capture.Get());sink::RgbaImage output;
                        require(sink::readRgba8(f.ctx,f.ring,f.graph->videoFrameResource(out.videoSlot),output),"read product output pixels");
                        const auto prefix=std::string("frame-")+std::to_string(n-o.warmup);const auto cp=pixelDir/(prefix+"-capture.png"),op=pixelDir/(prefix+"-output.png");
                        require(sink::saveImage(cp.wstring(),capture)&&sink::saveImage(op.wstring(),output),"save capture/output PNG evidence");
                        result.pixels.push_back({p.sequence,sha256Hex(capture.pixels.data(),capture.pixels.size()),sha256Hex(output.pixels.data(),output.pixels.size()),utf8(cp.wstring()),utf8(op.wstring())});}}
                out={};collect();
            }
            result.slotWaits=f.ring.cpuWaitCount()-startWaits;result.slotWaitMs=f.ring.cpuWaitMilliseconds()-startWaitMs;result.commandSubmits=f.ring.submitCount()-startSubmits;
            const auto drainStart=Clock::now();require(f.ring.drainQueue(),"drain final producer work");result.finalDrainMs=std::chrono::duration<double,std::milli>(Clock::now()-drainStart).count();collect();
            for(const auto& [id,s]:samples)result.samples.push_back(s);
            const auto m=f.graph->metrics();result.vsrEvaluations=m.srEvaluateCount;result.nrEvaluations=m.nrEvaluateCount;result.fgGenerated=m.fgGeneratedFrames;result.beautyEvaluations=m.beautyEvaluateCount;
            result.deviceAlive=f.ctx.checkDeviceAlive(result.removedReason);f.ctx.videoMemoryInfo(result.vramBudget,result.vramUsage);
            require(result.deviceAlive,"D3D12 device removed");require(result.vsrEvaluations==uint64_t(o.warmup)+o.frames&&result.nrEvaluations==0&&result.fgGenerated==0&&result.beautyEvaluations==0,"actual provider evaluation counts violate fixed test configuration");
            require(result.samples.size()==o.frames,"missing GPU timestamp frames");for(const auto& s:result.samples)require(s.sr>=0&&s.color>=0&&s.graph>=0&&s.inputRender>=0,"incomplete GPU stage sample");
            require(!o.readback||result.pixels.size()==o.frames,"missing pixel evidence frames");result.passed=true;
        }catch(const std::exception& e){result.error=e.what();result.passed=false;if(f.ctx.initialized()){result.deviceAlive=f.ctx.checkDeviceAlive(result.removedReason);result.adapter=f.ctx.adapter();}}
        f.close();
    }
    if(ownCom)CoUninitialize();result.elapsedMs=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
    try{writeResult(o,result);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 3;}return result.passed?0:1;
}
