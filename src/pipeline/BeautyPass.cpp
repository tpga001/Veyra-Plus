#include "veyra/pipeline/BeautyPass.h"
#include <wincodec.h>
#include <shlwapi.h>
#include <bit>
#include <cstring>
#include <array>
namespace veyra::pipeline {
namespace {
#include "BeautyLutData.inc"
struct Image {UINT width=0,height=0;std::vector<uint8_t> bytes;};
bool png(const uint8_t* data,size_t size,Image& image) {
    if(size>UINT_MAX)return false;
    ComPtr<IStream> stream;stream.Attach(SHCreateMemStream(data,UINT(size)));
    ComPtr<IWICImagingFactory> factory;
    if(!stream||FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))))return false;
    ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> convert;
    if(FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder))||
       FAILED(decoder->GetFrame(0,&frame))||FAILED(frame->GetSize(&image.width,&image.height))||
       !image.width||!image.height||image.width>1024||image.height>1024||
       FAILED(factory->CreateFormatConverter(&convert))||
       FAILED(convert->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return false;
    image.bytes.resize(size_t(image.width)*image.height*4);
    return SUCCEEDED(convert->CopyPixels(nullptr,image.width*4,UINT(image.bytes.size()),image.bytes.data()));
}
}
bool BeautyPass::initialize(ID3D12Device* device,uint32_t width,uint32_t height,ID3D12Resource* source) {
    close();
    if(GetEnvironmentVariableW(L"VEYRA_TEST_BEAUTY_INIT_REFUSE",nullptr,0))return false; // diagnostic refusal, no memory pressure
    if(!device||!source||!width||!height||source->GetDesc().Format!=DXGI_FORMAT_R16G16B16A16_FLOAT||
       source->GetDesc().Width!=width||source->GetDesc().Height!=height)return false;
    source_=source;width_=width;height_=height;
    horizontal_=makeTexture(device,width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
    mean_=makeTexture(device,width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
    output_=makeTexture(device,width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
    if(!horizontal_||!mean_||!output_)return false;
    std::vector<uint8_t> shader;
    if(!blur_.loadShader("BeautyBlur.dxil",shader)||!blur_.create(device,shader,4,1,1))return false;
    if(!apply_.loadShader("BeautyApply.dxil",shader)||!apply_.create(device,shader,7,6,1))return false;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const auto finish=[&](){if(SUCCEEDED(com))CoUninitialize();};
    if(FAILED(com)&&com!=RPC_E_CHANGED_MODE)return false;
    const uint8_t* bytes[]={kBeautyLut_gray,kBeautyLut_origin,kBeautyLut_skin,kBeautyLut_light};
    const size_t sizes[]={sizeof(kBeautyLut_gray),sizeof(kBeautyLut_origin),sizeof(kBeautyLut_skin),sizeof(kBeautyLut_light)};
    for(unsigned i=0;i<4;++i){
        Image image;if(!png(bytes[i],sizes[i],image)){finish();close();return false;}
        auto& lut=luts_[i];lut.texture=makeTexture(device,image.width,image.height,DXGI_FORMAT_R8G8B8A8_UNORM,false);
        if(!lut.texture){finish();close();return false;}
        const auto td=lut.texture->GetDesc();UINT rows;UINT64 rowBytes,total;
        device->GetCopyableFootprints(&td,0,1,0,&lut.footprint,&rows,&rowBytes,&total);
        lut.upload=makeUploadBuffer(device,total);void* mapped=nullptr;
        if(!lut.upload||FAILED(lut.upload->Map(0,nullptr,&mapped))){finish();close();return false;}
        for(UINT y=0;y<image.height;++y)std::memcpy(static_cast<uint8_t*>(mapped)+lut.footprint.Offset+size_t(y)*lut.footprint.Footprint.RowPitch,image.bytes.data()+size_t(y)*image.width*4,size_t(image.width)*4);
        D3D12_RANGE empty{};lut.upload->Unmap(0,&empty);
    }
    finish();
    DescriptorStager stager;if(!stager.initialize(device,8))return false;
    stager.stageSrv(source,nullptr,blur_.heap.Get(),0);
    stager.stageSrv(horizontal_.Get(),nullptr,blur_.heap.Get(),2);
    makeUav(device,horizontal_.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,cpuHandleOf(blur_,1));
    makeUav(device,mean_.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,cpuHandleOf(blur_,3));
    stager.stageSrv(source,nullptr,apply_.heap.Get(),0);
    stager.stageSrv(mean_.Get(),nullptr,apply_.heap.Get(),1);
    for(unsigned i=0;i<4;++i)stager.stageSrv(luts_[i].texture.Get(),nullptr,apply_.heap.Get(),i+2);
    makeUav(device,output_.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,cpuHandleOf(apply_,6));
    return true;
}
bool BeautyPass::run(ID3D12GraphicsCommandList* list,StateTracker& tracker,const engine::BeautySettings& settings) {
    if(!list||!settings.valid()||!settings.hasEffect()||!apply_.pso)return false;
    if(!uploaded_){
        for(auto& lut:luts_){
            tracker.transition(list,lut.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=lut.texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource=lut.upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=lut.footprint;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            tracker.transition(list,lut.texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        uploaded_=true;
    }
    tracker.transition(list,source_,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(settings.smoothing>0){
        for(unsigned i=0;i<2;++i){
            auto* input=i?horizontal_.Get():source_;auto* output=i?mean_.Get():horizontal_.Get();
            tracker.transition(list,input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            tracker.transition(list,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const float c[8]={std::bit_cast<float>(width_),std::bit_cast<float>(height_),std::bit_cast<float>(i),0,0,0,0,0};
            blur_.bind(list,c,gpuHandleOf(blur_,i*2).ptr,gpuHandleOf(blur_,i*2+1).ptr);
            list->Dispatch((width_+7)/8,(height_+7)/8,1);tracker.uavBarrier(list,output);
        }
    }
    tracker.transition(list,mean_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list,output_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float c[8]={std::bit_cast<float>(width_),std::bit_cast<float>(height_),settings.smoothing,settings.whitening,settings.detail,0,0,0};
    apply_.bind(list,c,gpuHandleOf(apply_,0).ptr,gpuHandleOf(apply_,6).ptr);
    list->Dispatch((width_+7)/8,(height_+7)/8,1);tracker.uavBarrier(list,output_.Get());
    ++evaluations_;return true;
}
void BeautyPass::close() {
    blur_={};apply_={};horizontal_.Reset();mean_.Reset();output_.Reset();luts_={};
    source_=nullptr;width_=height_=0;uploaded_=false;evaluations_=0;
}
}
