#include "veyra/engine/Subtitles.h"
#include <cmath>
#include <filesystem>
#include <iostream>
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    namespace fs=std::filesystem;
    const fs::path fixture=argv[1];
    int failures=0;
    auto check=[&](bool pass,const char* name){std::cout<<(pass?"PASS ":"FAIL ")<<name<<'\n';failures+=!pass;};
    for(auto [file,expected]:{std::pair{L"early.srt",2000},std::pair{L"late.srt",-1200}}){
        auto track=veyra::engine::loadSubtitleFile((fixture/file).wstring());
        check(track.cues.size()==5,"independently authored audio activity/cues");
        const auto first=veyra::engine::alignSubtitleToAudio((fixture/L"alignment.wav").wstring(),track,5);
        std::cout<<"offset="<<first.offsetMs<<" score="<<first.score<<'\n';
        check(first.ok&&std::abs(first.offsetMs-expected)<=40,"alignment returns correct signed total offset");
        track.offsetMs=expected>0?500:-400;
        const auto withPrevious=veyra::engine::alignSubtitleToAudio((fixture/L"alignment.wav").wstring(),track,5);
        check(withPrevious.ok&&std::abs(withPrevious.offsetMs-expected)<=40,"existing offset is preserved in total alignment");
        track.offsetMs=withPrevious.offsetMs;
        const auto repeated=veyra::engine::alignSubtitleToAudio((fixture/L"alignment.wav").wstring(),track,5);
        check(repeated.ok&&std::abs(repeated.offsetMs-expected)<=40,"repeated alignment does not lose previous offset");
        std::stop_source cancel;cancel.request_stop();
        check(!veyra::engine::alignSubtitleToAudio((fixture/L"alignment.wav").wstring(),track,5,cancel.get_token()).ok,"pre-cancelled alignment");
    }
    return failures?1:0;
}
