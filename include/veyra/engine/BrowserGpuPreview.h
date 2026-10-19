#pragma once
#include "veyra/engine/BrowserPreviewTypes.h"
#include "veyra/pipeline/FrameBatch.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/gfx/PresentSink.h"
#include <deque>
namespace veyra::gfx { class D3D12DeviceContext; class CommandSlotRing; }
namespace veyra::sink { struct RgbaImage; }
namespace veyra::engine {
// Engine-worker owner. Borrows the GUI-owned HWND until shutdown completes.
// The primary present command list/fence also covers this optional second draw.
class BrowserGpuPreview {
public:
    ~BrowserGpuPreview(){close();}
    void request(BrowserPreviewRequest,uint64_t activeSessionId,bool playing);
    void sourceProcessed(pipeline::FrameIdentity);
    void invalidateGeneration();
    bool prepare(gfx::D3D12DeviceContext&,ID3D12CommandQueue*,DXGI_FORMAT,bool hdr,HWND primaryWindow);
    bool record(ID3D12GraphicsCommandList*,pipeline::GraphicsPass&,D3D12_GPU_DESCRIPTOR_HANDLE,
                uint32_t sourceWidth,uint32_t sourceHeight,pipeline::FrameIdentity,bool highQuality,
                ID3D12Resource* reference,D3D12_GPU_DESCRIPTOR_HANDLE referenceSrv,bool referencesValid);
    void submitted(ID3D12Fence*,uint64_t);
    void present();
    BrowserPreviewSnapshot snapshot();
    void close();
    // Integration diagnostics only. Caller already drains both presenting and
    // shared queues; normal playback never calls this GPU-to-CPU path.
    bool readPresentedFrameForTest(gfx::D3D12DeviceContext&,gfx::CommandSlotRing&,sink::RgbaImage&);
private:
    void poll();
    void fail(const char* operation,HRESULT,LPCWSTR text);
    BrowserPreviewRequest request_{};
    BrowserPreviewSnapshot snapshot_{};
    gfx::PresentSink sink_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvs_;
    UINT rtvIncrement_=0,lastSuccessfulBuffer_=0,recordedBuffer_=0;
    bool initialized_=false,recorded_=false,hasSuccessfulPresent_=false;
    bool playing_=false,generationValid_=false;
    pipeline::FrameIdentity graphIdentity_{};
    uint64_t generation_=0,firstSourceFrameId_=0;
    struct Pending {
        BrowserPreviewRequest request;
        pipeline::FrameIdentity identity;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t value=0,generation=0;
        int comparisonMode=0;
        float comparisonSplit=.5f;
        bool referenceAvailable=false;
    };
    Pending recordedFrame_{};
    std::deque<Pending> pending_; // bounded by three, never an unbounded frame queue
};
} // namespace veyra::engine
