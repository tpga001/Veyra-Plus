#pragma once
#include "veyra/pipeline/GpuPassUtils.h"
#include <array>
#include "veyra/engine/BeautySettings.h"
namespace veyra::pipeline {
// One stateless SDR pass per real graph frame. LUT uploads are immutable,
// staged once on the same queue; no frame CPU readback or CPU wait.
class BeautyPass {
public:
    bool initialize(ID3D12Device*,uint32_t width,uint32_t height,ID3D12Resource* source);
    bool run(ID3D12GraphicsCommandList*,StateTracker&,const engine::BeautySettings&);
    ID3D12Resource* result() const {return output_.Get();}
    ID3D12Resource* input() const {return source_;}
    uint64_t evaluations() const {return evaluations_;}
    void close();
private:
    friend struct BeautyGpuTestAccess; // synthetic diagnostic access; no runtime or layout change
    struct Lut {ComPtr<ID3D12Resource> texture,upload;D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};};
    ComputePass blur_,apply_;
    ComPtr<ID3D12Resource> horizontal_,mean_,output_;
    std::array<Lut,4> luts_;
    ID3D12Resource* source_=nullptr;
    uint32_t width_=0,height_=0;
    bool uploaded_=false;
    uint64_t evaluations_=0;
};
}
