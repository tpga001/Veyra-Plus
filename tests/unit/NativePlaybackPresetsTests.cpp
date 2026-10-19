// Acceptance of starter presets through the real on-disk codec and application
// boundary. No GPU, display timing, audio device, or user configuration is used.
#include "veyra/engine/NativePlaybackPresets.h"
#include "veyra/engine/PlaybackDisplayPolicy.h"
#include "veyra/engine/GraphDescription.h"
#include "veyra/engine/ExportFeaturePolicy.h"
#include "veyra/engine/PresetStore.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

using namespace veyra::engine;
namespace {
int failures = 0;
unsigned checks = 0;
void check(bool ok, const std::string& label) {
    ++checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}
const NativePlaybackPreset& choice(const std::vector<NativePlaybackPreset>& choices,
                                  const std::string& id) {
    const auto found = std::find_if(choices.begin(), choices.end(),
        [&](const auto& preset) { return preset.id == id; });
    if (found == choices.end()) throw std::runtime_error("missing preset: " + id);
    return *found;
}
const PresetEntry& entry(const PresetLibrary& library, const std::wstring& name) {
    const auto found = std::find_if(library.entries().begin(), library.entries().end(),
        [&](const auto& preset) { return preset.name == name; });
    if (found == library.entries().end()) throw std::runtime_error("missing saved preset");
    return *found;
}
using DiskSnapshot = std::map<std::filesystem::path, std::string>;
DiskSnapshot files(const std::filesystem::path& directory) {
    DiskSnapshot result;
    for (const auto& item : std::filesystem::directory_iterator(directory)) {
        if (!item.is_regular_file()) continue;
        std::ifstream stream(item.path(), std::ios::binary);
        result.emplace(item.path().filename(), std::string(std::istreambuf_iterator<char>(stream), {}));
    }
    return result;
}
EnhancementSettings userSettings() {
    EnhancementSettings settings;
    settings.audioSync = AudioSyncMode::Manual;
    settings.audioOffsetMs = -137;
    settings.exportBitrateMbps = 173;
    settings.captureCompatible = true;
    settings.captureAudio = CaptureAudioIngress::BitstreamPreferred;
    settings.captureBuffer = veyra::source::CaptureBufferMode::DriverDefault;
    settings.captureFlipVertical = true;
    settings.forceSdrPreview = true;
    settings.color.enabled = true;
    settings.color.exposure = 0.75f;
    settings.color.setLutName(L"acceptance-user-look.cube");
    settings.additionalColorCount = 1;
    settings.additionalColors[0].enabled = true;
    settings.additionalColors[0].exposure = -0.375f;
    settings.multiplier = 3;
    settings.frameGenerationBackend = FrameGenerationBackend::Dlss;
    settings.srTarget = veyra::pipeline::SrTarget::Uhd8K;
    return settings;
}
bool unrelatedSettingsEqual(const EnhancementSettings& a, const EnhancementSettings& b) {
    // Volume belongs to the UI/audio session, not this settings/preset contract.
    // These are nontrivial settings that could otherwise silently change when
    // fromChain() and the optional globals snapshot are both applied.
    return a.audioSync == b.audioSync && a.audioOffsetMs == b.audioOffsetMs &&
        a.exportBitrateMbps == b.exportBitrateMbps &&
        a.captureCompatible == b.captureCompatible && a.captureAudio == b.captureAudio &&
        a.captureBuffer == b.captureBuffer && a.captureFlipVertical == b.captureFlipVertical &&
        a.forceSdrPreview == b.forceSdrPreview && a.revision == b.revision &&
        a.color == b.color && a.additionalColorCount == b.additionalColorCount &&
        a.additionalColors == b.additionalColors;
}
struct Applied {
    EnhancementSettings settings;
    EffectChain chain;
};
Applied applyForPlan(const PresetEntry& preset) {
    Applied applied;
    applied.settings = userSettings();
    applied.chain = toChain(applied.settings);
    const auto validation = PresetLibrary::applyToChain(preset, applied.chain, applied.settings);
    if (!validation.accepted) throw std::runtime_error("preset application rejected: " + std::string(validation.message));
    return applied;
}
ChainExecutionPlan planFor(const Applied& applied, veyra::pipeline::Extent source) {
    ChainExecutionRequest request;
    request.source = source;
    request.srTarget = applied.settings.srTarget;
    request.srPlaybackExtent = applied.settings.srPlaybackExtent;
    ChainExecutionPlan plan;
    const auto validation = compileChainExecutionPlan(applied.chain, request, plan);
    if (!validation.accepted) throw std::runtime_error("execution plan rejected: " + std::string(validation.message));
    return plan;
}
const ChainExecutionStep* step(const ChainExecutionPlan& plan, EffectType type) {
    for (uint32_t i = 0; i < plan.stepCount; ++i)
        if (plan.steps[i].type == type) return &plan.steps[i];
    return nullptr;
}
void testRoundtrip(const std::filesystem::path& directory,
                   const std::vector<NativePlaybackPreset>& choices) {
    const auto path = directory / L"native-presets.v1";
    PresetLibrary writer(path);
    writer.setIncludeBuiltins(false);
    check(writer.load() && writer.entries().empty(), "isolated empty library loads without builtins");
    std::set<std::string> ids;
    std::set<std::wstring> names;
    bool unique = !choices.empty();
    for (const auto& preset : choices) {
        unique = unique && ids.insert(preset.id).second && names.insert(preset.entry.name).second;
        check(writer.put(preset.entry, false), "real codec accepts and saves " + preset.id);
    }
    check(choices.size() == 7 && unique, "seven independently addressable starter choices");
    check(!writer.defaultIndex(), "adding choices does not elect a default");
    check(writer.save(), "complete starter library saves");
    const auto expected = writer.entries();
    PresetLibrary reader(path); // Deliberately the original path, not the schema sidecar.
    reader.setIncludeBuiltins(false);
    check(reader.load() && !reader.corrupt() && reader.entries() == expected,
          "fresh original-path reader restores complete Unicode payloads and optional globals");
    check(!files(directory).empty() && !reader.defaultIndex(),
          "roundtrip uses actual files and retains no-default selection");

    for (const auto& preset : choices) {
        const auto& restored = entry(reader, preset.entry.name);
        const auto before = userSettings();
        auto flat = before;
        PresetLibrary::apply(restored, flat);
        auto applied = applyForPlan(restored);
        check(flat == applied.settings && flat.validate().empty(),
              "flat and list application agree after reload: " + preset.id);
        check(unrelatedSettingsEqual(before, applied.settings),
              "preserves audio sync offset export capture and user colour: " + preset.id);
        check(!(restored.contents & presetContentMask(PresetContent::Audio)),
              "starter never claims user's audio settings: " + preset.id);
    }

    const auto original = applyForPlan(entry(reader, choice(choices, "original").entry.name));
    check(!original.settings.sr && !original.settings.nr && original.settings.multiplier == 1,
          "original choice bypasses NR SR and frame generation even from enhanced state");
    for (const auto* id : {"xess-only", "smooth-2k", "smooth-4k", "nr720-2k"}) {
        const auto applied = applyForPlan(entry(reader, choice(choices, id).entry.name));
        const auto plan = planFor(applied, {1920, 1080});
        check(applied.settings.multiplier == 2 &&
              applied.settings.frameGenerationBackend == FrameGenerationBackend::XeSS &&
              step(plan, EffectType::FrameGeneration) && plan.stepCount &&
              plan.steps[plan.stepCount - 1].type == EffectType::FrameGeneration &&
              applied.settings.content == ContentRate::Transport,
              std::string("2X explicitly selects XeSS and uses source cadence with FG last: ") + id);
    }
    const auto xessOnly = applyForPlan(entry(reader, choice(choices, "xess-only").entry.name));
    const auto xessOnlyPlan = planFor(xessOnly, {1920, 1080});
    check(!xessOnly.settings.nr && !xessOnly.settings.sr &&
          xessOnlyPlan.output == veyra::pipeline::Extent{1920, 1080} &&
          !step(xessOnlyPlan, EffectType::SuperResolution) &&
          !step(xessOnlyPlan, EffectType::NrEnhance),
          "XeSS-only control retains source resolution and bypasses SR and NR");
    const auto qhd = applyForPlan(entry(reader, choice(choices, "vsr4-2k").entry.name));
    const auto uhd = applyForPlan(entry(reader, choice(choices, "vsr4-4k").entry.name));
    check(qhd.settings.videoSrQuality == 4 && uhd.settings.videoSrQuality == 4 &&
          planFor(qhd, {1920, 1080}).output == veyra::pipeline::Extent{2560, 1440} &&
          planFor(uhd, {1920, 1080}).output == veyra::pipeline::Extent{3840, 2160},
          "1080p processing plans select maximum VSR quality and distinct 2K/4K extents");
    check(planFor(qhd, {1920, 800}).output == veyra::pipeline::Extent{2560, 1066} &&
          planFor(uhd, {1920, 800}).output == veyra::pipeline::Extent{3840, 1600},
          "2K/4K targets preserve cinema aspect ratio with even texture extents");
    const auto alreadyLarge = planFor(qhd, {3840, 2160});
    check(alreadyLarge.output == veyra::pipeline::Extent{3840, 2160} &&
          !step(alreadyLarge, EffectType::SuperResolution),
          "2K target is an upscale bound and does not downscale a 4K source");

    const auto& nrEntry = entry(reader, choice(choices, "nr720-2k").entry.name);
    const auto nr = applyForPlan(nrEntry);
    const auto hdPlan = planFor(nr, {1920, 1080});
    const auto fourKPlan = planFor(nr, {3840, 2160});
    const auto* hdNr = step(hdPlan, EffectType::NrEnhance);
    const auto* fourKNr = step(fourKPlan, EffectType::NrEnhance);
    const auto* hdSr = step(hdPlan, EffectType::SuperResolution);
    check(nr.settings.nr && nr.settings.nrRuntime == NrRuntime::Ampere && hdNr && fourKNr &&
          hdNr->processing == veyra::pipeline::Extent{1280, 720} &&
          fourKNr->processing == veyra::pipeline::Extent{1280, 720} &&
          hdNr->processing != veyra::pipeline::Extent{960, 540} &&
          fourKNr->processing != veyra::pipeline::Extent{1920, 1080},
          "NR720 is a 720p processing bound rather than 50 percent of source dimensions");
    check(hdNr && hdSr && hdNr < hdSr &&
          hdNr->input == veyra::pipeline::Extent{1920, 1080} &&
          hdNr->output == hdNr->input && hdSr->input == hdNr->output &&
          hdPlan.output == veyra::pipeline::Extent{2560, 1440},
          "experimental NR processes before SR while retaining full-size stage output");
    check(nrEntry.note.find(L"720p") != std::wstring::npos &&
          nrEntry.note.find(L"NR50") != std::wstring::npos,
          "experimental choice explicitly documents NR720 versus NR50");
}
void testCollisions(const std::filesystem::path& directory,
                    const std::vector<NativePlaybackPreset>& choices) {
    const auto path = directory / L"user-presets.v1";
    PresetLibrary library(path);
    library.setIncludeBuiltins(false);
    check(library.load(), "collision fixture loads isolated library");
    auto user = choice(choices, "smooth-2k").entry;
    user.note = L"user-owned preset, retain custom processing";
    user.fg = {1, FrameGenerationBackend::Fsr, 1};
    auto custom = userSettings();
    custom.sr = true;
    custom.videoSrQuality = 2;
    custom.multiplier = 1;
    user.chain = toChain(custom);
    user.globals = ChainGlobalSettings::capture(custom);
    check(library.put(user, false) && library.setDefault(0),
          "user-owned same-name preset and explicit default persist");
    const auto originalDefault = library.defaultIndex();
    for (const auto& preset : choices) {
        if (preset.entry.name == user.name) continue;
        check(library.put(preset.entry, false), "nonconflicting starter adds: " + preset.id);
    }
    const auto expected = library.entries();
    const auto expectedDisk = files(directory);
    bool refused = true;
    for (unsigned repeat = 0; repeat < 3; ++repeat)
        for (const auto& preset : choices)
            refused = !library.put(preset.entry, false) && refused;
    check(refused && library.entries() == expected && library.defaultIndex() == originalDefault &&
          files(directory) == expectedDisk,
          "repeated same-name adds are atomic and leave entries default and bytes unchanged");
    PresetLibrary reader(path);
    reader.setIncludeBuiltins(false);
    check(reader.load() && reader.entries().size() == choices.size() &&
          reader.entries() == expected && entry(reader, user.name) == user &&
          reader.defaultIndex() && reader.entries()[*reader.defaultIndex()].name == user.name,
          "restart retains custom same-name preset and user's chosen default");
}

void testSourceDefaults(const std::filesystem::path& directory) {
    auto base = userSettings();
    base.sr = false;
    base.videoSrQuality = 2;
    base.beauty = {false, .28f, .0042f, .17f};
    base.srMotion = MotionSource::Zero;
    base.videoHdr.enabled = true;
    base.videoHdr.peakNits = 900;
    const auto local = sourcePlaybackDefaults(base, PlaybackSourceProfile::LocalVideo);
    auto expectedLocal = base;
    expectedLocal.sr = true; expectedLocal.videoSrQuality = 4; expectedLocal.beauty.enabled = true;
    check(local == expectedLocal, "local defaults change only SR4 and Beauty master while retaining every other setting");
    const auto browser = sourcePlaybackDefaults(local, PlaybackSourceProfile::Browser);
    auto expectedBrowser = local;
    expectedBrowser.videoSrQuality = 0; expectedBrowser.beauty.enabled = false;
    expectedBrowser.srMotion = MotionSource::OpticalFlow;
    check(browser == expectedBrowser, "browser defaults select DLSS motion and Beauty off without losing NR FG HDR or strengths");
    check(local.beauty.smoothing == browser.beauty.smoothing &&
          local.beauty.whitening == browser.beauty.whitening && local.beauty.detail == browser.beauty.detail,
          "fine whitening and other Beauty parameters remain independent source seeds");

    const auto choices = sourcePlaybackPresets(local, browser);
    check(choices.size() == 2 && choices[0].id == "local-default" && choices[1].id == "browser-default" &&
          choices[0].entry.name != choices[1].entry.name, "source preset picker offers two friendly independently addressable choices");
    PresetLibrary library(directory / L"source-presets.v1");
    library.setIncludeBuiltins(false);
    check(library.load(), "source preset library loads isolated fixture");
    for (const auto& choice : choices) check(library.put(choice.entry), "source preset writes " + choice.id);
    PresetLibrary reload(directory / L"source-presets.v1");
    reload.setIncludeBuiltins(false);
    check(reload.load() && reload.entries() == library.entries() && !reload.defaultIndex(),
          "two Unicode source choices codec-roundtrip without electing a shared startup default");
    for (const auto& choice : choices) {
        auto applied = base;
        PresetLibrary::apply(choice.entry, applied);
        const auto expected = choice.id == "local-default" ? local : browser;
        check(applied.beauty == expected.beauty && applied.sr && applied.videoSrQuality == expected.videoSrQuality &&
              applied.srMotion == expected.srMotion && applied.nr == base.nr &&
              applied.multiplier == base.multiplier && applied.frameGenerationBackend == base.frameGenerationBackend &&
              applied.videoHdr == base.videoHdr && unrelatedSettingsEqual(base, applied),
              "friendly source preset retains NR FG HDR colour audio and fine Beauty strengths: " + choice.id);
    }

    const auto localPath = directory / L"chain-session.v1";
    const auto browserPath = directory / L"browser-chain-session.v1";
    ChainSessionStore localStore(localPath), browserStore(browserPath);
    auto localSession = ChainSession::initial(local);
    auto browserSession = ChainSession::initial(browser);
    check(localStore.save(localSession) && browserStore.save(browserSession),
          "local and browser use separate session codec files");
    auto changed = browser;
    changed.videoSrQuality = 2; changed.beauty.enabled = true; changed.beauty.whitening = .0063f;
    browserSession = ChainSession::initial(changed);
    check(browserStore.save(browserSession), "browser manual settings save separately");
    ChainSession restoredLocal, restoredBrowser;
    ChainSessionStore localReader(localPath), browserReader(browserPath);
    check(localReader.load(restoredLocal) && browserReader.load(restoredBrowser) &&
          restoredLocal == localSession && restoredBrowser == browserSession,
          "restart restores local Beauty-on VSR4 and independent browser manual settings");
    EnhancementSettings restored;
    restoredLocal.configurations[0].apply(restored);
    check(restored.videoSrQuality == 4 && restored.beauty.enabled &&
          restored.beauty.whitening == .0042f,
          "browser modifications cannot change local fine Beauty or SR selection");
    // A damaged independent browser file cannot authorize replacing it.
    const auto badPath = directory / L"corrupt-browser.v1";
    { std::ofstream bad(badPath, std::ios::binary); bad << "bad browser profile"; }
    ChainSessionStore badStore(badPath);
    auto untouched = localSession;
    check(!badStore.load(untouched) && untouched == localSession && !badStore.save(browserSession),
          "corrupt browser profile refuses load/save without modifying caller session");
    check(localReader.load(restoredLocal) && restoredLocal == localSession,
          "browser corruption leaves the local player session usable and unchanged");
}

void testDisplayPolicy(const std::filesystem::path& directory,
                       const std::vector<NativePlaybackPreset>& choices) {
    using veyra::pipeline::Extent;
    using veyra::pipeline::NrSizePolicy;
    using veyra::pipeline::ResolutionPlan;
    using veyra::pipeline::SrTarget;
    struct DisplayFixture { const char* label; Extent display, expected; };
    const DisplayFixture displays[] {
        {"main 4K monitor", {3840,2160}, {3840,2160}},
        {"portrait secondary monitor", {1080,1920}, {1080,1920}},
        {"4:3 secondary monitor", {2048,1536}, {2048,1536}},
        {"ultrawide monitor", {3440,1440}, {3440,1440}},
        {"wide monitor beyond cost cap", {5120,1440}, {3840,1080}},
        {"large 4:3 monitor", {4096,3072}, {2880,2160}},
        {"portrait 8K monitor", {4320,7680}, {2160,3840}},
        {"odd physical extent rounds down", {1919,1079}, {1918,1078}},
        {"missing display", {}, {}},
        {"partial missing display", {0,1080}, {}},
        {"out-of-range display", {65535,1080}, {}},
        {"display too small for even pixels", {1,1}, {}}
    };
    for (const auto& f : displays)
        check(boundedPlaybackDisplayExtent(f.display) == f.expected,
              std::string("physical display policy: ") + f.label);

    auto manual = userSettings();
    manual.sr = true; manual.nr = false; manual.multiplier = 1; manual.videoSrQuality = 4;
    const auto before = manual;
    const auto playback = displayPlaybackSettings(manual, {1080,1920}, true, true);
    auto withoutProjection = playback; withoutProjection.srPlaybackExtent = {};
    check(manual == before && withoutProjection == manual &&
          playback.srTarget == SrTarget::Uhd8K && playback.srPlaybackExtent == Extent{1080,1920},
          "playback clone changes only runtime extent and retains manual target audio colour capture and export");
    check(displayPlaybackSettings(playback, {3840,2160}, false, true) == manual,
          "turning display following off clears an existing runtime limit");
    check(displayPlaybackSettings(playback, {3840,2160}, true, false) == manual,
          "image Node and other non-list-video contexts clear an existing runtime limit");
    check(displayPlaybackSettings(manual, {}, true, true) == manual,
          "unknown display falls back to manual rather than a guessed 4K target");
    const auto mainPlayback = displayPlaybackSettings(manual, {3840,2160}, true, true);
    check(!playback.sameVideoConfiguration(mainPlayback) &&
          requiresGraphRebuild(playback, mainPlayback) && !requiresGraphRebuild(playback, playback),
          "cross-monitor change rebuilds the graph while an unchanged extent is a no-op");
    auto audioOnly = playback; audioOnly.audioOffsetMs += 25;
    check(playback.sameVideoConfiguration(audioOnly) && !requiresGraphRebuild(playback, audioOnly),
          "audio-only editing retains display projection without rebuilding video");
    auto srOff = playback; srOff.sr = false;
    const auto srOffMain = displayPlaybackSettings(srOff, {3840,2160}, true, true);
    const auto srOffPortrait = displayPlaybackSettings(srOff, {1080,1920}, true, true);
    check(srOffMain.srPlaybackExtent == Extent{} && srOffPortrait.srPlaybackExtent == Extent{} &&
          srOffMain == srOffPortrait && !requiresGraphRebuild(srOffMain, srOffPortrait),
          "original and XeSS-only playback do not rebuild when moved between displays with SR off");

    struct PlanFixture { const char* label; Extent display, source, expected; };
    const PlanFixture plans[] {
        {"720p on 1080p", {1920,1080}, {1280,720}, {1920,1080}},
        {"portrait video on portrait display", {1080,1920}, {720,1280}, {1080,1920}},
        {"4:3 video on 4:3 display", {2048,1536}, {640,480}, {2048,1536}},
        {"landscape on 4:3 display", {2048,1536}, {1280,720}, {2048,1152}},
        {"square video retains aspect", {2048,1536}, {500,500}, {1536,1536}},
        {"ordinary ultrawide retains video aspect", {3440,1440}, {1280,720}, {2560,1440}},
        {"wide display cost cap", {5120,1440}, {1280,720}, {1920,1080}},
        {"native 1080p skips identity SR", {1920,1080}, {1920,1080}, {1920,1080}},
        {"4K source never silently downsampled", {1920,1080}, {3840,2160}, {3840,2160}},
        {"source wider than portrait remains native", {1080,1920}, {1280,720}, {1280,720}}
    };
    for (const auto& f : plans) {
        Applied applied; applied.settings = displayPlaybackSettings(manual, f.display, true, true);
        applied.chain = toChain(applied.settings);
        const auto plan = planFor(applied, f.source);
        StageRequest request; request.width = f.source.width; request.height = f.source.height; request.sr = true;
        veyra::pipeline::EnhanceGraphDesc desc;
        const auto resolution = describeStages(request, applied.settings, desc);
        check(plan.output == f.expected && resolution.output == f.expected &&
              desc.workWidth == f.expected.width && desc.workHeight == f.expected.height &&
              desc.fixedExecutionPlanError.empty() && desc.fixedExecutionPlan &&
              desc.fixedExecutionPlan->output == plan.output &&
              desc.enableSr == (f.expected != f.source) &&
              bool(step(plan, EffectType::SuperResolution)) == (f.expected != f.source),
              std::string("compiled plan graph and identity admission agree: ") + f.label);
    }
    auto nr = applyForPlan(choice(choices, "nr720-2k").entry);
    nr.settings = displayPlaybackSettings(nr.settings, {2048,1536}, true, true);
    const auto nrPlan = planFor(nr, {1920,1080});
    const auto* nrStep = step(nrPlan, EffectType::NrEnhance);
    const auto* srStep = step(nrPlan, EffectType::SuperResolution);
    check(nrStep && srStep && nrStep < srStep && nrStep->processing == Extent{1280,720} &&
          nrStep->output == Extent{1920,1080} && srStep->input == nrStep->output &&
          srStep->output == Extent{2048,1152} && nrPlan.output == Extent{2048,1152},
          "exact display target preserves NR720-before-SR with full-source residual composition");

    StageRequest offline; offline.width = 1280; offline.height = 720; offline.sr = true; offline.exportJob = true;
    veyra::pipeline::EnhanceGraphDesc exportDesc;
    const auto exportPlan = describeStages(offline, playback, exportDesc);
    const auto frozen = freezeExportSettings(playback);
    check(frozen.srPlaybackExtent == Extent{} && frozen.srTarget == manual.srTarget &&
          frozen.audioOffsetMs == manual.audioOffsetMs && exportPlan.output == Extent{7680,4320} &&
          exportDesc.fixedExecutionPlan && exportDesc.fixedExecutionPlan->output == exportPlan.output,
          "export freezes manual 8K target and strips an accidentally supplied portrait projection");
    offline.exportJob = false; offline.stillImage = true;
    veyra::pipeline::EnhanceGraphDesc imageDesc;
    const auto imagePlan = describeStages(offline, playback, imageDesc);
    check(imagePlan.output == Extent{7680,4320} && imageDesc.fixedExecutionPlan &&
          imageDesc.fixedExecutionPlan->output == imagePlan.output,
          "still-image processing ignores a stale playback monitor projection");

    for (const auto bad : {Extent{0,1080}, Extent{1920,0}, Extent{16385,1080}}) {
        auto invalid = manual; invalid.srPlaybackExtent = bad;
        bool rejected = false;
        try { (void)ResolutionPlan::make({1280,720}, true, NrSizePolicy::Native, false, 0, SrTarget::Uhd8K, false, bad); }
        catch (const std::invalid_argument&) { rejected = true; }
        ChainExecutionRequest request; request.source = {1280,720};
        request.srTarget = manual.srTarget; request.srPlaybackExtent = bad;
        ChainExecutionPlan unchanged; unchanged.output = {123,456};
        const auto verdict = compileChainExecutionPlan(toChain(manual), request, unchanged);
        StageRequest graphRequest; graphRequest.width = 1280; graphRequest.height = 720; graphRequest.sr = true;
        veyra::pipeline::EnhanceGraphDesc invalidDesc;
        const auto invalidPlan = describeStages(graphRequest, invalid, invalidDesc);
        check(!invalid.validate().empty() && rejected && !verdict.accepted &&
              unchanged.output == Extent{123,456} && !invalidDesc.fixedExecutionPlanError.empty() &&
              !invalidDesc.fixedExecutionPlan && invalidPlan.output == Extent{},
              "invalid runtime extent rejected atomically: " + std::to_string(bad.width) + "x" + std::to_string(bad.height));
    }

    // Exercise real isolated files. A monitor move must not change saved preset bytes.
    const auto legacyPath = directory / L"legacy-display-presets.v1";
    PresetStore legacy(legacyPath);
    check(legacy.load() && legacy.put(L"manual 8K with runtime display", playback) && legacy.save(),
          "legacy codec accepts valid projected settings in isolated storage");
    const auto firstDisk = files(directory);
    check(legacy.put(L"manual 8K with runtime display", mainPlayback, true) && legacy.save() && files(directory) == firstDisk,
          "changing runtime screen extent leaves legacy preset bytes unchanged");
    PresetStore legacyReader(legacyPath);
    check(legacyReader.load() && legacyReader.entries().size() == 1 &&
          legacyReader.entries()[0].settings.srPlaybackExtent == Extent{} &&
          legacyReader.entries()[0].settings.srTarget == manual.srTarget &&
          legacyReader.entries()[0].settings.audioOffsetMs == manual.audioOffsetMs,
          "legacy restart restores manual target and excludes playback display projection");

    auto persistent = choice(choices, "smooth-4k").entry;
    persistent.name = L"isolated manual target"; persistent.chain = toChain(playback);
    persistent.globals = ChainGlobalSettings::capture(playback);
    persistent.fg = {playback.multiplier, playback.frameGenerationBackend, playback.vfgQuality};
    persistent.flow = PresetFlowSettings::capture(playback);
    const auto libraryPath = directory / L"display-library.v1";
    PresetLibrary library(libraryPath); library.setIncludeBuiltins(false);
    check(library.load() && library.put(persistent, false) && library.save(),
          "current preset library saves manual globals captured from projected playback");
    PresetLibrary libraryReader(libraryPath); libraryReader.setIncludeBuiltins(false);
    auto restoredPreset = manual; restoredPreset.srPlaybackExtent = {};
    const bool loadedLibrary = libraryReader.load() && libraryReader.entries().size() == 1;
    if (loadedLibrary) PresetLibrary::apply(libraryReader.entries()[0], restoredPreset);
    check(loadedLibrary && restoredPreset.srPlaybackExtent == Extent{} &&
          restoredPreset.srTarget == manual.srTarget && restoredPreset.audioOffsetMs == manual.audioOffsetMs,
          "current preset codec restores manual target keeping player audio and excluding runtime extent");

    const auto sessionPath = directory / L"display-chain-session.v1";
    ChainSessionStore sessionStore(sessionPath);
    const auto session = ChainSession::initial(playback);
    check(session == ChainSession::initial(mainPlayback) && sessionStore.save(session),
          "mode-session payload is independent of playback display");
    auto restoredSession = ChainSession::initial(manual);
    const bool loadedSession = sessionStore.load(restoredSession);
    auto restoredSettings = manual;
    if (loadedSession) restoredSession.configurations[size_t(restoredSession.active)].apply(restoredSettings);
    check(loadedSession && restoredSettings.srPlaybackExtent == Extent{} &&
          restoredSettings.srTarget == manual.srTarget && restoredSession == session,
          "real mode-session restart keeps manual target and excludes runtime display state");
}

}
int main(int argc, char** argv) {
    try {
        const auto parent = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path();
        // Never read the real preset library. Every invocation has its own path,
        // retained on disk so CI or a reviewer can inspect roundtrip evidence.
        const auto scratch = parent / ("native-presets-acceptance-" +
            std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(scratch / "roundtrip");
        std::filesystem::create_directories(scratch / "collisions");
        const auto choices = nativePlaybackPresets();
        testRoundtrip(scratch / "roundtrip", choices);
        testCollisions(scratch / "collisions", choices);
        std::filesystem::create_directories(scratch / "display-policy");
        testDisplayPolicy(scratch / "display-policy", choices);
        std::filesystem::create_directories(scratch / "source-defaults");
        testSourceDefaults(scratch / "source-defaults");
        std::printf("RESULT %u checks, %d failures\n", checks, failures);
        std::printf("ARTIFACTS %s\n", scratch.string().c_str());
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::printf("FAIL exception: %s\n", error.what());
        return 1;
    }
}


