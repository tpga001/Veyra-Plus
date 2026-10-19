#include "veyra/engine/Subtitles.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using veyra::engine::SubtitleTrack;
using Clock = std::chrono::steady_clock;
int failures = 0, checks = 0;
void check(bool ok, const char* name) {
    ++checks; failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
}
void writeBytes(const fs::path& path, const std::string& bytes) {
    if (fs::exists(path)) throw std::runtime_error("refuse fixture overwrite");
    std::ofstream out(path, std::ios::binary);
    if (!out || !out.write(bytes.data(), std::streamsize(bytes.size())))
        throw std::runtime_error("fixture write failed");
}
std::string srt(const std::string& text) {
    return "1\n00:00:01,000 --> 00:00:03,000\n" + text +
           "\n\n2\n00:00:04,000 --> 00:00:06,000\nSECOND " + text + "\n";
}
std::string vtt(const std::string& text) {
    return "WEBVTT\n\n00:00:01.000 --> 00:00:03.000\n" + text +
           "\n\n00:00:04.000 --> 00:00:06.000\nSECOND " + text + "\n";
}
std::string ass(const std::string& text, bool ssa = false) {
    const std::string header =
        "[Script Info]\nScriptType: " + std::string(ssa ? "v4.00" : "v4.00+") +
        "\nPlayResX: 640\nPlayResY: 360\n" +
        std::string(ssa ? "[V4 Styles]\n" : "[V4+ Styles]\n") +
        "Format: Name, Fontname, Fontsize, PrimaryColour\n"
        "Style: Default,Arial,22,&H00FFFFFF\n[Events]\n"
        "Format: Layer, Start, End, Style, Text\n";
    return header + "Dialogue: 0,0:00:01.00,0:00:03.00,Default," + text +
        "\nDialogue: 0,0:00:04.00,0:00:06.00,Default,SECOND " + text + "\n";
}
void writeUtf16(const fs::path& path, const std::wstring& text) {
    std::string bytes("\xFF\xFE", 2);
    for (const auto c : text) {
        bytes.push_back(char(unsigned(c) & 255));
        bytes.push_back(char((unsigned(c) >> 8) & 255));
    }
    writeBytes(path, bytes);
}
struct Expected {
    std::wstring file, language, text;
    bool fullAss = false;
};
const SubtitleTrack* named(const std::vector<SubtitleTrack>& tracks, const std::wstring& file) {
    const auto it = std::find_if(tracks.begin(), tracks.end(),
        [&](const auto& t) { return t.name.find(file) != std::wstring::npos; });
    return it == tracks.end() ? nullptr : &*it;
}
veyra::engine::SubtitleLoader::Result awaitResult(veyra::engine::SubtitleLoader& loader,
                                                 uint64_t generation) {
    const auto until = Clock::now() + std::chrono::seconds(20);
    while (Clock::now() < until) {
        if (auto r = loader.poll()) {
            if (r->generation != generation)
                throw std::runtime_error("stale subtitle generation published");
            if (r->complete) return std::move(*r);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("subtitle loader timeout");
}
bool hasCue(const std::vector<SubtitleTrack>& tracks, const std::wstring& text) {
    for (const auto& t : tracks) for (const auto& cue : t.cues)
        if (cue.text == text) return true;
    return false;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "Pass the isolated native-stage4 fixture root\n"; return 2;
    }
    try {
        const fs::path root = fs::absolute(argv[1]);
        if (!fs::is_directory(root))
            throw std::runtime_error("fixture root must already exist");
        const auto nonce = std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(Clock::now().time_since_epoch().count());
        const auto cases = root / (L"外挂字幕 独立验收-" + nonce);
        if (!fs::create_directory(cases))
            throw std::runtime_error("refuse existing test case directory");
        const std::wstring stem = L"中文片名 独立Test";
        const fs::path media = cases / (stem + L".mp4"); // No media is read by direct discovery.
        std::vector<Expected> expected;
        auto add = [&](const std::wstring& suffix, const std::wstring& language,
                       const std::string& text, int format = 0) {
            const auto name = stem + suffix;
            writeBytes(cases / name, format == 1 ? ass(text) :
                format == 2 ? ass(text, true) : format == 3 ? vtt(text) : srt(text));
            expected.push_back({name, language, std::wstring(text.begin(), text.end()),
                                format == 1 || format == 2});
        };
        add(L".srt", L"", "EXACT SRT");
        add(L".ASS", L"", "EXACT ASS", 1);
        add(L".ssa", L"", "EXACT SSA", 2);
        add(L".vtt", L"", "EXACT VTT", 3);
        add(L".zh-CN.srt", L"zh-CN", "SIMPLIFIED");
        add(L".en.ASS", L"en", "ENGLISH ASS", 1);
        add(L".ja.vtt", L"ja", "JAPANESE", 3);
        add(L".en.forced.srt", L"en", "FORCED ENGLISH");
        add(L".zh-CN.sdh.srt", L"zh-CN", "SDH CHINESE");
        add(L".en.cc.vtt", L"en", "CC ENGLISH", 3);
        const auto utf16Name = stem + L".zh-TW.srt";
        const std::wstring utf16Text = L"繁體字幕與中文路徑";
        writeUtf16(cases / utf16Name,
            L"1\r\n00:00:01,000 --> 00:00:03,000\r\n" + utf16Text + L"\r\n");
        expected.push_back({utf16Name, L"zh-TW", utf16Text});

        // Valid-looking unrelated subtitles must never join this movie's track list.
        for (const auto& suffix : {L"2.en.srt", L" 第二部.en.srt", L".review.srt",
                                  L".en.other.srt", L".en.forced.extra.srt", L".srt.bak"})
            writeBytes(cases / (stem + suffix), srt("MUST NOT LOAD"));
        writeBytes(cases / L"另一段片名.zh-CN.srt", srt("MUST NOT LOAD"));
        writeBytes(cases / (stem + L".fr.srt"), "");
        writeBytes(cases / (stem + L".de.srt"), "bad text without authored cue timestamps\n");
        const auto sub = cases / L"子目录";
        fs::create_directory(sub);
        writeBytes(sub / (stem + L".ko.srt"), srt("MUST NOT LOAD"));

        const auto tracks = veyra::engine::loadExternalSubtitleTracks(media.wstring());
        check(tracks.size() == expected.size(), "all matching valid external tracks, and only those tracks");
        for (const auto& want : expected) {
            const auto* t = named(tracks, want.file);
            check(t != nullptr, "authored matching filename is listed");
            if (!t) continue;
            check(t->language == want.language, "language metadata uses the independent expected language");
            check(!t->embedded && t->usable() && !t->codec.empty(), "external parsed track is usable");
            check(veyra::engine::textAt(*t, 2.0) == want.text, "decoded text equals independently authored content");
            check(veyra::engine::textAt(*t, 0.5).empty() &&
                  veyra::engine::textAt(*t, 3.0).empty(), "cue visibility obeys media timestamps and end exclusivity");
            check(bool(t->ass) == want.fullAss, "ASS/SSA original script is retained without changing text formats");
        }
        bool exactOrder = tracks.size() >= 4;
        for (size_t i = 0; exactOrder && i < 4; ++i)
            exactOrder &= tracks[i].name.find(expected[i].file) != std::wstring::npos;
        check(exactOrder, "exact same-name formats retain SRT ASS SSA VTT default priority");
        const auto repeat = veyra::engine::loadExternalSubtitleTracks(media.wstring());
        bool stable = repeat.size() == tracks.size();
        for (size_t i = 0; stable && i < tracks.size(); ++i)
            stable &= repeat[i].name == tracks[i].name && repeat[i].language == tracks[i].language;
        check(stable, "repeated directory scans return deterministic track order");
        check(!hasCue(tracks, L"MUST NOT LOAD"), "shared prefixes unknown tags backups and nested directories are excluded");

        std::stop_source stop; stop.request_stop();
        const auto cancelStart = Clock::now();
        check(veyra::engine::loadExternalSubtitleTracks(media.wstring(), stop.get_token()).empty(),
              "pre-cancelled external discovery publishes no tracks");
        check(Clock::now() - cancelStart < std::chrono::seconds(2),
              "pre-cancelled discovery returns promptly");
        check(veyra::engine::loadExternalSubtitleTracks(L"capture:fixture").empty() &&
              veyra::engine::loadExternalSubtitleTracks(L"screen:fixture").empty() &&
              veyra::engine::loadExternalSubtitleTracks(L"").empty(),
              "non-file sources do not discover filesystem subtitle tracks");

        // The two inputs are explicitly generated test fixtures, never user videos.
        const auto fixtureA = root / L"linkage-a.mp4", fixtureB = root / L"linkage-b.mp4";
        bool loaderVerified = false;
        if (fs::is_regular_file(fixtureA) && fs::is_regular_file(fixtureB)) {
            const auto mediaA = cases / L"换片测试 A.mp4", mediaB = cases / L"换片测试 B.mp4";
            fs::copy_file(fixtureA, mediaA);
            fs::copy_file(fixtureB, mediaB);
            auto sideA = mediaA; sideA.replace_extension(L".srt");
            auto sideB = mediaB; sideB.replace_extension(L".srt");
            writeBytes(sideA, srt("ONLY MOVIE A"));
            writeBytes(sideB, srt("ONLY MOVIE B"));
            veyra::engine::SubtitleLoader loader;
            loader.request(mediaA.wstring());
            const auto cancelAt = Clock::now();
            const auto cancelled = awaitResult(loader, loader.request(L""));
            check(cancelled.tracks.empty(), "cancelled media load cannot retain previous subtitle tracks");
            check(Clock::now() - cancelAt < std::chrono::seconds(2), "pending media cancellation remains bounded");
            const auto loadedA = awaitResult(loader, loader.request(mediaA.wstring()));
            check(hasCue(loadedA.tracks, L"ONLY MOVIE A"), "real media loader discovers movie A sidecar");
            loader.request(mediaA.wstring());
            const auto loadedB = awaitResult(loader, loader.request(mediaB.wstring()));
            check(hasCue(loadedB.tracks, L"ONLY MOVIE B") && !hasCue(loadedB.tracks, L"ONLY MOVIE A"),
                  "rapid file switch publishes the new generation and never old movie text");
            check(awaitResult(loader, loader.request(L"screen:fixture")).tracks.empty(),
                  "switching to a non-file source clears discovered tracks");
            loaderVerified = true;
        } else {
            std::cout << "SKIP real-media generation test: linkage-a.mp4 and linkage-b.mp4 are absent\n";
        }
        std::wcout << L"FIXTURE_CASE_DIRECTORY=" << cases.wstring() << L'\n';
        std::cout << "CHECKS=" << checks << " FAILURES=" << failures
                  << " MEDIA_GENERATION_VERIFIED=" << loaderVerified << '\n';
        return failures ? 1 : (loaderVerified ? 0 : 3);
    } catch (const std::exception& e) {
        std::cerr << "FAIL exception: " << e.what() << '\n'; return 1;
    }
}
