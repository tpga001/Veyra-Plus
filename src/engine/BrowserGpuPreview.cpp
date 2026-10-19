#include "veyra/engine/BrowserGpuPreview.h"
#include "veyra/engine/PreviewView.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/sink/ImageExportSink.h"
#include "veyra/Log.h"
#include <algorithm>
#include <format>

namespace veyra::engine {
namespace {
bool ownedChild(HWND window) {
    DWORD process=0;
    return window&&IsWindow(window)&&GetWindowThreadProcessId(window,&process)&&
        process==GetCurrentProcessId()&&(GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD)&&
        GetParent(window)&&IsWindow(GetParent(window));
}
}
void BrowserGpuPreview::request(BrowserPreviewRequest request,uint64_t sessionId,bool playing) {
    if(!browserPreviewRevisionAccepted(request_,request)){
        playing_=false;snapshot_.live=false;return;
    }
    const bool sameSource=request_.window==request.window&&request_.sessionId==request.sessionId;
    const bool changed=request_!=request;
    if(!sameSource)snapshot_={};
    else if(changed)snapshot_.failed=false;
    request_=request;playing_=playing&&browserPreviewRequestMatches(request,sessionId);
    snapshot_.window=request.window;snapshot_.sessionId=request.sessionId;snapshot_.revision=request.revision;snapshot_.generation=generation_;
    poll();
    if(!playing_) {
        snapshot_.live=false;
        if(!snapshot_.failed){snapshot_.state=snapshot_.hasFrame?BrowserPreviewState::Frozen:BrowserPreviewState::Disabled;
            snapshot_.status=snapshot_.hasFrame?L"预览已冻结":L"预览未开启";}
    } else if(changed&&!snapshot_.failed) {
        snapshot_.live=false;snapshot_.state=BrowserPreviewState::Waiting;
        snapshot_.status=L"等待增强画面预览";
    }
}
void BrowserGpuPreview::invalidateGeneration(){
    ++generation_;generationValid_=false;pending_.clear();recorded_=false;recordedFrame_={};
    snapshot_.hasFrame=false;snapshot_.live=false;snapshot_.sourceFrameId=0;
    snapshot_.epoch=0;snapshot_.settingsRevision=0;snapshot_.generation=generation_;
    snapshot_.referenceAvailable=false;snapshot_.frameRequestRevision=0;
    if(!snapshot_.failed){snapshot_.state=BrowserPreviewState::Waiting;snapshot_.status=L"等待当前处理阶段的新画面";}
}
void BrowserGpuPreview::sourceProcessed(pipeline::FrameIdentity identity){
    if(!generationValid_||identity.epoch!=graphIdentity_.epoch||identity.settingsRevision!=graphIdentity_.settingsRevision){
        invalidateGeneration();firstSourceFrameId_=identity.sourceFrameId;
    }
    graphIdentity_=identity;generationValid_=identity.settingsRevision!=0;
}
void BrowserGpuPreview::fail(const char* operation,HRESULT result,LPCWSTR text) {
    snapshot_.live=false;snapshot_.failed=true;snapshot_.state=BrowserPreviewState::Failed;snapshot_.status=text;
    log::warn("browser-preview",std::format("operation={} hr=0x{:X} window={} session={} revision={} primaryContinues=1",
        operation,unsigned(result),request_.window,request_.sessionId,request_.revision));
}
bool BrowserGpuPreview::prepare(gfx::D3D12DeviceContext& ctx,ID3D12CommandQueue* queue,
                                DXGI_FORMAT format,bool hdr,HWND primaryWindow) {
    recorded_=false;poll();
    if(!playing_||snapshot_.failed||!generationValid_)return false;
    const HWND window=reinterpret_cast<HWND>(request_.window);
    if(window==primaryWindow||!ownedChild(window)||!IsWindowVisible(window)||
       IsIconic(GetAncestor(window,GA_ROOT))) {
        snapshot_.live=false;snapshot_.state=BrowserPreviewState::Unavailable;
        snapshot_.status=L"预览窗口不可见";return false;
    }
    if(hdr||format!=DXGI_FORMAT_R8G8B8A8_UNORM) {
        snapshot_.hasFrame=false;snapshot_.live=false;snapshot_.state=BrowserPreviewState::Unsupported;
        snapshot_.referenceAvailable=false;snapshot_.frameRequestRevision=0;
        snapshot_.status=L"HDR 暂不支持同步预览，网页增强继续";return false;
    }
    if(initialized_&&sink_.hwnd()!=window) {
        // Target replacement is exceptional. Ordinary hide/pause/window resize
        // never gets here and never drains/rebuilds the secondary swapchain.
        sink_.shutdown();rtvs_.Reset();pending_.clear();initialized_=false;hasSuccessfulPresent_=false;
    }
    if(!initialized_) {
        gfx::PresentSink::Desc desc;desc.targetWindow=window;desc.width=1280;desc.height=720;
        desc.vsync=false;desc.tearing=false;desc.waitable=true;desc.xess=false;desc.fgMultiplier=1;
        desc.previewOnly=true;Status status=Status::Ok;
        if(!sink_.initialize(ctx.device(),queue,desc,status)) {
            fail("initialize",E_FAIL,L"预览显示初始化失败，网页增强继续");return false;
        }
        if(!sink_.configurePacing(true,false,false)) {
            fail("capacity-contract",E_NOTIMPL,L"预览容量检查不可用，网页增强继续");return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=3;
        const HRESULT created=ctx.device()->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&rtvs_));
        if(FAILED(created)){fail("rtv-heap",created,L"预览资源创建失败，网页增强继续");return false;}
        rtvIncrement_=ctx.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for(UINT i=0;i<3;++i) {
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
            const HRESULT fetched=sink_.swapChain()->GetBuffer(i,IID_PPV_ARGS(&buffer));
            if(FAILED(fetched)){fail("backbuffer",fetched,L"预览资源获取失败，网页增强继续");return false;}
            ctx.device()->CreateRenderTargetView(buffer.Get(),nullptr,
                {rtvs_->GetCPUDescriptorHandleForHeapStart().ptr+size_t(i)*rtvIncrement_});
        }
        initialized_=true;snapshot_.width=1280;snapshot_.height=720;
        log::info("browser-preview",std::format("created window={} session={} buffer=1280x720 fg=0 readback=0",
            request_.window,request_.sessionId));
    }
    // No image work is recorded when the secondary output lacks capacity.
    // presentationReady polls its waitable event with a zero-millisecond wait.
    if(pending_.size()>=3||!sink_.presentationReady()) {
        ++snapshot_.skippedBusy;snapshot_.live=false;snapshot_.state=BrowserPreviewState::Busy;
        snapshot_.status=L"预览繁忙，已跳过；网页增强继续";return false;
    }
    return true;
}
bool BrowserGpuPreview::record(ID3D12GraphicsCommandList* list,pipeline::GraphicsPass& pass,
    D3D12_GPU_DESCRIPTOR_HANDLE source,uint32_t sourceWidth,uint32_t sourceHeight,
    pipeline::FrameIdentity identity,bool highQuality,ID3D12Resource* reference,
    D3D12_GPU_DESCRIPTOR_HANDLE referenceSrv,bool referencesValid) {
    if(!initialized_||!playing_||snapshot_.failed||!generationValid_||!list||!sourceWidth||!sourceHeight||
       !browserPreviewGenerationMatches(graphIdentity_.epoch,graphIdentity_.settingsRevision,firstSourceFrameId_,
                                        identity.epoch,identity.settingsRevision,identity.sourceFrameId))return false;
    const HWND window=reinterpret_cast<HWND>(request_.window);
    RECT client{};if(!GetClientRect(window,&client)||client.right<1||client.bottom<1)return false;
    uint32_t bufferIndex=0;auto* buffer=sink_.currentBackBuffer(&bufferIndex);
    if(!buffer){fail("acquire-backbuffer",E_FAIL,L"预览缓冲不可用，网页增强继续");return false;}
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=buffer;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1,&barrier);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvs_->GetCPUDescriptorHandleForHeapStart().ptr+size_t(bufferIndex)*rtvIncrement_};
    const float black[4]{0,0,0,1};list->ClearRenderTargetView(rtv,black,0,nullptr);list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    const PreviewView view{};
    const auto [renderWidth,renderHeight]=view.renderedSize(float(client.right),float(client.bottom),
        float(sourceWidth),float(sourceHeight));
    const float inverseFit=1.0f/std::min(float(client.right)/renderWidth,float(client.bottom)/renderHeight);
    float dimensions[16]{renderWidth,renderHeight,highQuality?2.0f:0.0f,inverseFit,
        float(client.right),float(client.bottom),view.centerX,view.centerY};
    const D3D12_VIEWPORT viewport{0,0,float(sink_.bufferWidth()),float(sink_.bufferHeight()),0,1};
    D3D12_RECT rectangle{0,0,LONG(sink_.bufferWidth()),LONG(sink_.bufferHeight())};
    list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&rectangle);
    ID3D12DescriptorHeap* heaps[]{pass.heap.Get()};list->SetDescriptorHeaps(1,heaps);
    list->SetGraphicsRootSignature(pass.rootSig.Get());list->SetPipelineState(pass.pso.Get());
    list->SetGraphicsRoot32BitConstants(0,16,dimensions,0);list->SetGraphicsRootDescriptorTable(1,source);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
    // sourceReference belongs to this same real-frame slot and final producer
    // handoff. Invalid references never sample a previous parity's pixels.
    const bool referenceAvailable=referencesValid&&reference&&referenceSrv.ptr;
    const int actualMode=referenceAvailable?request_.comparisonMode:0;
    if(actualMode!=0){
        D3D12_RESOURCE_BARRIER referenceBarrier{};
        referenceBarrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        referenceBarrier.Transition.pResource=reference;
        referenceBarrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        referenceBarrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;
        referenceBarrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1,&referenceBarrier);
        // The original reference is linear FP16; the enhanced SDR frame is
        // already encoded. Use the existing sRGB transfer, with identical UVs.
        dimensions[2]=(highQuality?2.0f:0.0f)+1.0f;
        list->SetGraphicsRoot32BitConstants(0,16,dimensions,0);
        list->SetGraphicsRootDescriptorTable(1,referenceSrv);
        if(actualMode==2){
            const float boundary=float(client.right)*.5f+(request_.comparisonSplit-.5f)*renderWidth;
            rectangle.right=LONG(std::clamp(boundary*float(sink_.bufferWidth())/float(client.right),
                0.0f,float(sink_.bufferWidth())));
        }
        if(rectangle.right>rectangle.left){
            list->RSSetScissorRects(1,&rectangle);list->DrawInstanced(3,1,0,0);
        }
        std::swap(referenceBarrier.Transition.StateBefore,referenceBarrier.Transition.StateAfter);
        list->ResourceBarrier(1,&referenceBarrier);
        if(actualMode==2&&request_.comparisonSplit>0.0f&&request_.comparisonSplit<1.0f){
            // The browser preview is SDR. Draw only its visible comparison
            // guide; the primary browser surface and graph textures stay exact.
            rectangle={0,0,LONG(sink_.bufferWidth()),LONG(sink_.bufferHeight())};
            list->RSSetScissorRects(1,&rectangle);
            dimensions[2]=64.0f;dimensions[14]=request_.comparisonSplit;
            dimensions[15]=std::max(1.0f,float(GetDpiForWindow(window))/96.0f);
            list->SetGraphicsRoot32BitConstants(0,16,dimensions,0);
            list->SetGraphicsRootDescriptorTable(1,source);
            list->DrawInstanced(3,1,0,0);
        }
    }
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
    recordedFrame_={request_,identity,{},0,generation_,actualMode,request_.comparisonSplit,referenceAvailable};
    recorded_=true;recordedBuffer_=bufferIndex;
    return true;
}
void BrowserGpuPreview::submitted(ID3D12Fence* fence,uint64_t value) {
    if(!recorded_)return;
    recordedFrame_.fence=fence;recordedFrame_.value=value;
}
void BrowserGpuPreview::present() {
    if(!recorded_)return;
    recorded_=false;Status status=Status::Ok;
    const auto result=sink_.tryPresentPreview(status);
    if(result==gfx::PresentSink::PreviewPresentResult::Presented) {
        ++snapshot_.submitted;hasSuccessfulPresent_=true;lastSuccessfulBuffer_=recordedBuffer_;
        if(recordedFrame_.fence&&recordedFrame_.value&&pending_.size()<3)pending_.push_back(recordedFrame_);
    } else if(result==gfx::PresentSink::PreviewPresentResult::Busy) {
        ++snapshot_.skippedBusy;snapshot_.live=false;snapshot_.state=BrowserPreviewState::Busy;
        snapshot_.status=L"预览繁忙，已跳过；网页增强继续";
    } else if(result==gfx::PresentSink::PreviewPresentResult::Occluded) {
        snapshot_.live=false;snapshot_.state=BrowserPreviewState::Unavailable;snapshot_.status=L"预览被遮挡";
    } else fail("nonblocking-present",E_FAIL,L"预览显示失败，网页增强继续");
    recordedFrame_={};poll();
}
void BrowserGpuPreview::poll() {
    while(!pending_.empty()) {
        const auto& front=pending_.front();
        const auto completed=front.fence?front.fence->GetCompletedValue():0;
        if(completed==UINT64_MAX){fail("completed-fence",DXGI_ERROR_DEVICE_REMOVED,L"预览设备不可用，网页增强继续");pending_.clear();return;}
        if(completed<front.value)break;
        if(generationValid_&&front.generation==generation_&&
           browserPreviewGenerationMatches(graphIdentity_.epoch,graphIdentity_.settingsRevision,firstSourceFrameId_,
               front.identity.epoch,front.identity.settingsRevision,front.identity.sourceFrameId)&&
           browserPreviewFrameReady(request_,front.request,completed,front.value,true)) {
            ++snapshot_.gpuCompleted;snapshot_.hasFrame=true;snapshot_.live=playing_&&!snapshot_.failed;
            snapshot_.sourceFrameId=front.identity.sourceFrameId;snapshot_.epoch=front.identity.epoch;
            snapshot_.settingsRevision=front.identity.settingsRevision;
            snapshot_.comparisonMode=front.comparisonMode;snapshot_.comparisonSplit=front.comparisonSplit;
            snapshot_.referenceAvailable=front.referenceAvailable;snapshot_.frameRequestRevision=front.request.revision;
            if(playing_&&!snapshot_.failed){
                const bool referencePending=front.request.comparisonMode!=0&&!front.referenceAvailable;
                snapshot_.state=referencePending?BrowserPreviewState::Waiting:BrowserPreviewState::Ready;
                snapshot_.status=referencePending?L"原图待就绪，显示当前增强画面":
                    front.comparisonMode==1?L"实时增强前预览 · 不含 XeSS 内部生成帧":
                    front.comparisonMode==2?L"左侧增强前 · 右侧增强后 · 不含 XeSS 内部生成帧":
                    L"实时增强预览 · 不含 XeSS 内部生成帧";
            }
        }
        pending_.pop_front();
    }
}
BrowserPreviewSnapshot BrowserGpuPreview::snapshot(){poll();return snapshot_;}
bool BrowserGpuPreview::readPresentedFrameForTest(gfx::D3D12DeviceContext& ctx,
    gfx::CommandSlotRing& ring,sink::RgbaImage& image) {
    if(!hasSuccessfulPresent_||!sink_.swapChain())return false;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    return SUCCEEDED(sink_.swapChain()->GetBuffer(lastSuccessfulBuffer_,IID_PPV_ARGS(&buffer)))&&
        sink::readRgba8(ctx,ring,buffer.Get(),image);
}
void BrowserGpuPreview::close() {
    recorded_=false;recordedFrame_={};
    // PresentSink drains post-Present queue work before releasing backbuffers.
    // This runs only at ordered teardown/target replacement, never per frame.
    sink_.shutdown();rtvs_.Reset();pending_.clear();initialized_=false;hasSuccessfulPresent_=false;
    snapshot_={};request_={};playing_=false;generationValid_=false;graphIdentity_={};generation_=0;firstSourceFrameId_=0;
}
} // namespace veyra::engine
