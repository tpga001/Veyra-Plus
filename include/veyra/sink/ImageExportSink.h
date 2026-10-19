#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <d3d12.h>
namespace veyra::gfx { class D3D12DeviceContext; class CommandSlotRing; }
namespace veyra::sink {
struct RgbaImage { uint32_t width=0, height=0; std::vector<uint8_t> pixels; };
// Image export / explicit diagnostics only. Never used by playback or video export.
bool readRgba8(gfx::D3D12DeviceContext&, gfx::CommandSlotRing&, ID3D12Resource*, RgbaImage&);
// Task-scoped explicit SDR diagnostic only; FP16 linear reference to sRGB PNG codes.
bool readLinearSdrRgba8ForDiagnostic(gfx::D3D12DeviceContext&, gfx::CommandSlotRing&, ID3D12Resource*, RgbaImage&);
bool saveImage(const std::wstring& path, const RgbaImage&, bool jpeg=false);
bool saveHdrScreenshot(const std::wstring& path,gfx::D3D12DeviceContext&,gfx::CommandSlotRing&,ID3D12Resource*);
bool loadImage(const std::wstring& path, RgbaImage&);
}
