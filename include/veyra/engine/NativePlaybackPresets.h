#pragma once
#include "veyra/engine/PresetLibrary.h"
#include <string_view>
namespace veyra::engine {
struct NativePlaybackPreset { std::string id; PresetEntry entry; };
// Explicit starter choices. They do not replace defaults, change display size,
// or claim that a requested backend has run successfully.
std::vector<NativePlaybackPreset> nativePlaybackPresets();
// Source-specific first-use defaults change only SR selection and Beauty's
// switch. All NR, FG, colour, HDR and user Beauty strengths stay in the base.
enum class PlaybackSourceProfile { LocalVideo, Browser };
EnhancementSettings sourcePlaybackDefaults(EnhancementSettings base, PlaybackSourceProfile source);
std::vector<NativePlaybackPreset> sourcePlaybackPresets(
    const EnhancementSettings& local, const EnhancementSettings& browser);
}
