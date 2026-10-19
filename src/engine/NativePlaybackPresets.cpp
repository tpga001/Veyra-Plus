#include "veyra/engine/NativePlaybackPresets.h"
namespace veyra::engine {
std::vector<NativePlaybackPreset> nativePlaybackPresets() {
    std::vector<NativePlaybackPreset> result;
    auto add=[&](const char* id,const wchar_t* name,const wchar_t* note,bool sr,
                 pipeline::SrTarget target,bool fg,bool nr) {
        EnhancementSettings s;
        s.sr=sr;s.videoSrQuality=4;s.srTarget=target;
        s.nr=nr;s.nrRuntime=NrRuntime::Ampere;
        s.nrPolicy=nr?pipeline::NrSizePolicy::P720:pipeline::NrSizePolicy::Realtime;
        s.lowLatency=nr; // Explicit NR before SR; not a change to the global default.
        s.multiplier=fg?2:1;s.frameGenerationBackend=FrameGenerationBackend::XeSS;
        s.srMotion=MotionSource::Zero;
        s.nrMotion=MotionSource::OpticalFlow;
        s.fgMotion=MotionSource::OpticalFlow;
        s.content=ContentRate::Transport;
        PresetEntry e;
        e.name=name;e.note=note;e.kind=ChainMode::List;e.builtin=false;
        // Preserve the user's audio and presentation settings.
        e.contents=presetContentMask(PresetContent::Chain)|
                   presetContentMask(PresetContent::FrameGeneration)|
                   presetContentMask(PresetContent::Flow);
        e.chain=toChain(s);e.fg={s.multiplier,s.frameGenerationBackend,s.vfgQuality};
        e.globals=ChainGlobalSettings::capture(s);
        e.flow=PresetFlowSettings::capture(s);
        result.push_back({id,std::move(e)});
    };
    add("original",L"本地 · 原画对照",L"关闭 NR、超分和补帧；保留音频与显示设置",
        false,pipeline::SrTarget::Qhd,false,false);
    add("vsr4-2k",L"本地 · VSR4 · 2K",L"最高档 VSR；内部上限 2560×1440，窗口大小不变",
        true,pipeline::SrTarget::Qhd,false,false);
    add("vsr4-4k",L"本地 · VSR4 · 4K",L"最高档 VSR；内部上限 3840×2160，窗口大小不变",
        true,pipeline::SrTarget::Uhd4K,false,false);
    add("xess-only",L"本地 · XeSS2× 对照",L"关闭 NR 与超分，仅验证 XeSS 2× 补帧",
        false,pipeline::SrTarget::Qhd,true,false);
    add("smooth-2k",L"本地 · 2K · VSR4＋XeSS2×",L"NR 关闭；30 帧理论到 60，24 帧理论到 48；实际输出需测量",
        true,pipeline::SrTarget::Qhd,true,false);
    add("smooth-4k",L"本地 · 4K · VSR4＋XeSS2×",L"NR 关闭；4K 内部处理，XeSS 2×；不等于稳定 60 帧",
        true,pipeline::SrTarget::Uhd4K,true,false);
    add("nr720-2k",L"本地 · NR720＋VSR4＋XeSS2×",L"实验 SF-v2 NR → VSR → XeSS；NR 内部上限 720p，非 Magpie NR50",
        true,pipeline::SrTarget::Qhd,true,true);
    return result;
}
EnhancementSettings sourcePlaybackDefaults(EnhancementSettings base, PlaybackSourceProfile source) {
    base.sr = true;
    base.videoSrQuality = source == PlaybackSourceProfile::Browser ? 0u : 4u;
    base.beauty.enabled = source == PlaybackSourceProfile::LocalVideo;
    // DLSS consumes estimated motion rather than the VSR diagnostic zero field.
    if (source == PlaybackSourceProfile::Browser) base.srMotion = MotionSource::OpticalFlow;
    return base;
}
std::vector<NativePlaybackPreset> sourcePlaybackPresets(
        const EnhancementSettings& local, const EnhancementSettings& browser) {
    std::vector<NativePlaybackPreset> result;
    const auto add = [&](const char* id, const wchar_t* name, const wchar_t* note,
                         const EnhancementSettings& base, PlaybackSourceProfile source) {
        const auto s = sourcePlaybackDefaults(base, source);
        PresetEntry entry;
        entry.name = name; entry.note = note; entry.kind = ChainMode::List; entry.builtin = false;
        entry.contents = presetContentMask(PresetContent::Chain) |
            presetContentMask(PresetContent::FrameGeneration) | presetContentMask(PresetContent::Flow);
        entry.chain = toChain(s);
        entry.fg = {s.multiplier, s.frameGenerationBackend, s.vfgQuality};
        entry.globals = ChainGlobalSettings::capture(s);
        entry.flow = PresetFlowSettings::capture(s);
        result.push_back({id, std::move(entry)});
    };
    add("local-default", L"本地 · RTX 视频超分＋美颜",
        L"RTX 视频超分最高档；开启美颜；NR、补帧与显示设置保留",
        local, PlaybackSourceProfile::LocalVideo);
    add("browser-default", L"浏览器 · DLSS SR",
        L"DLSS SR 超分；关闭美颜；独立保存浏览器参数，窗口大小不变",
        browser, PlaybackSourceProfile::Browser);
    return result;
}
}
