#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "veyra/diagnostics/SrProofPolicy.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <array>
#include <DirectXPackedVector.h>
#include "veyra/engine/EnhancementSettings.h"
using namespace veyra::diagnostics;
static std::vector<uint8_t> codeRaster(unsigned scale,std::array<uint16_t,4> words,unsigned low=0,unsigned high=255) {
    const unsigned width=1280*scale,height=720*scale;std::vector<uint8_t> pixels(size_t(width)*height*4,uint8_t(low));
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<16;++col){
        const unsigned value=(words[row]&(1u<<(15-col)))?high:low;
        for(unsigned y=(12+row*8)*scale;y<(20+row*8)*scale;++y)
            for(unsigned x=(12+col*8)*scale;x<(20+col*8)*scale;++x){const auto at=(size_t(y)*width+x)*4;
                pixels[at]=pixels[at+1]=pixels[at+2]=uint8_t(value);pixels[at+3]=255;}
    }
    return pixels;
}
int main() {
    unsigned checks=0,failures=0;
    auto check=[&](bool yes,const char* text){++checks;if(!yes){++failures;std::cout<<"FAIL "<<text<<'\n';}};
    const auto suffix=std::to_wstring(GetCurrentProcessId());
    const auto fixture=srProofTaskRoot()/L"fixtures"/(L"sr-proof-policy-cpu-"+suffix+L".mp4");
    const auto dir=srProofTaskRoot()/L"tests"/(L"sr-proof-policy-cpu-"+suffix);
    std::error_code error;std::filesystem::create_directories(fixture.parent_path(),error);
    HANDLE handle=CreateFileW(fixture.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(handle!=INVALID_HANDLE_VALUE,"exclusive task fixture");if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
    auto file=SrProofPolicy::parse(dir.wstring(),fixture.wstring(),L"");
    check(file.has_value(),"exact task-local source admitted");check(file&&file->frames==1,"default one frame");
    check(file&&file->allowsFile(fixture),"exact actual file admitted");
    check(file&&!file->allowsFile(fixture.parent_path()/L"other.mp4"),"different file rejected");
    check(!SrProofPolicy::parse((srProofTaskRoot()/L"outside").wstring(),fixture.wstring(),L"1"),"outside tests rejected");
    check(!SrProofPolicy::parse((srProofTaskRoot()/L"tests"/L".."/L"escape").wstring(),fixture.wstring(),L"1"),"dotdot output rejected");
    check(!SrProofPolicy::parse(L"relative",fixture.wstring(),L"1"),"relative output rejected");
    check(!SrProofPolicy::parse(dir.wstring(),L"C:/Users/private.mp4",L"1"),"non-task source rejected");
    for(const auto* count:{L"0",L"4",L"-1",L"01",L"1x"})check(!SrProofPolicy::parse(dir.wstring(),fixture.wstring(),count),"invalid count rejected");
    for(const auto* count:{L"1",L"2",L"3"})check(SrProofPolicy::parse(dir.wstring(),fixture.wstring(),count).has_value(),"bounded count accepted");
    auto window=SrProofPolicy::parse(dir.wstring(),L"screen-window:12345",L"3");
    check(window&&window->allowsWindow(12345,L"Veyra SR Proof - Chrome"),"explicit fixture window admitted");
    check(window&&!window->allowsWindow(12346,L"Veyra SR Proof - Chrome"),"wrong window rejected");
    check(window&&!window->allowsWindow(12345,L"Personal video - Chrome"),"wrong title rejected");
    check(window&&!window->allowsFile(fixture),"window authority cannot admit file");
    check(!SrProofPolicy::parse(dir.wstring(),L"screen-window:0",L"1"),"zero window rejected");
    check(!SrProofPolicy::parse(dir.wstring(),L"screen-window:18446744073709551616",L"1"),"window overflow rejected");
    check(!SrProofPolicy::parse(dir.wstring(),L"screen-window:+12345",L"1"),"nondecimal window rejected");
    check(SrProofPolicy::parse(dir.wstring(),fixture.wstring(),L"3",L"25,26,99").has_value(),"explicit local source IDs admitted");
    check(!SrProofPolicy::parse(dir.wstring(),L"screen-window:12345",L"1",L"25"),"compositor sequence cannot be media ID authority");
    for(const auto* ids:{L"0",L"25,",L"25,25",L"26,25",L"18446744073709551616"})check(!SrProofPolicy::parse(dir.wstring(),fixture.wstring(),L"1",ids),"invalid local source IDs rejected");
    check(!SrProofPolicy::parse(dir.wstring(),fixture.wstring(),L"2",L"25"),"source ID count must match frame limit");
    auto codes=SrProofPolicy::parse(dir.wstring(),L"screen-window:12345",L"3",L"",L"60,150,240");
    check(codes&&codes->codeIds==std::vector<unsigned>({60,150,240}),"explicit browser code IDs admitted separately");
    check(SrProofPolicy::parse(dir.wstring(),L"screen-window:12345",L"1",L"",L"0").has_value(),"browser code ID zero admitted");
    check(!SrProofPolicy::parse(dir.wstring(),fixture.wstring(),L"1",L"",L"60"),"browser code IDs cannot authorize local file");
    check(!SrProofPolicy::parse(dir.wstring(),L"screen-window:12345",L"3",L"",L"60,150"),"code ID count must match frame limit");
    for(const auto* ids:{L"300",L"-1",L"60,",L",60",L"60,60",L"150,60",L"60,150,240,299",L"60 150",L"18446744073709551616"})
        check(!SrProofPolicy::parse(dir.wstring(),L"screen-window:12345",L"1",L"",ids),"invalid browser code IDs rejected");
    check(codes&&codes->allowsWindow(12345,L"Veyra SR Proof - Chrome")&&!codes->allowsWindow(12345,L"Private video - Chrome"),"code selection retains fixture window authority");
    if(codes){
        auto changed=*codes;
        check(srProofCanResume(*codes,L"screen:fixture",4,*codes,L"screen:fixture",4,1,true,false),"released same-authority proof may resume");
        check(!srProofCanResume(*codes,L"screen:fixture",4,*codes,L"screen:fixture",4,2,true,false),"live engine ownership blocks resume");
        check(!srProofCanResume(*codes,L"screen:fixture",4,*codes,L"screen:fixture",0,1,true,false),"changed quality blocks resume");
        check(!srProofCanResume(*codes,L"screen:fixture",4,*codes,L"screen:other",4,1,true,false),"changed source blocks resume");
        check(!srProofCanResume(*codes,L"screen:fixture",4,*codes,L"screen:fixture",4,1,false,false),"changed settings recipe blocks resume");
        check(!srProofCanResume(*codes,L"screen:fixture",4,*codes,L"screen:fixture",4,1,true,true),"failed proof stays disarmed");
        changed.codeIds={0,150,240};check(!srProofCanResume(*codes,L"screen:fixture",4,changed,L"screen:fixture",4,1,true,false),"changed code selection blocks resume");
        changed=*codes;changed.window++;check(!srProofCanResume(*codes,L"screen:fixture",4,changed,L"screen:fixture",4,1,true,false),"changed target blocks resume");
        changed=*codes;changed.directory/=L"other";check(!srProofCanResume(*codes,L"screen:fixture",4,changed,L"screen:fixture",4,1,true,false),"changed directory cannot resume old proof");
        changed=*codes;changed.codeIds.clear();check(!srProofCanResume(changed,L"screen:fixture",4,changed,L"screen:fixture",4,1,true,false),"ordinary diagnostics cannot resume");
    }
    veyra::engine::EnhancementSettings recipe,nextRecipe;nextRecipe.revision=99;
    check(srProofSameRecipe(recipe,nextRecipe),"actual settings recipe excludes only revision");
    nextRecipe.srMotion=veyra::engine::MotionSource::Zero;check(!srProofSameRecipe(recipe,nextRecipe),"motion configuration change is a true recipe change");
    check(srProofRecipeHash("hello")==0xa430d84680aabd0bull,"canonical recipe fingerprint known vector");
    SrProofCounter resumed;resumed.limit=3;resumed.saved=2;resumed.observed=500;resumed.last=500;resumed.reopened();
    check(resumed.saved==2&&resumed.observed==0&&resumed.last==0,"reopen retains total saves but resets per-source warmup");
    for(uint64_t i=1;i<=24;++i)check(!resumed.candidate(i),"resumed source earns fresh 24-frame warmup");
    check(resumed.candidate(25),"resumed source can save only remaining sample");resumed.completed();
    check(!resumed.candidate(26)&&resumed.saved==3,"three-pair limit survives reopen");
    // Literal fixture words from independently CRC-validated PNG records.
    for(unsigned scale:{2u,3u}){
        auto raster=codeRaster(scale,{0xD3A5,0x203C,0xEC36,0xDFC3});
        auto decoded=srProofDecodeCode(raster,1280*scale,720*scale);
        check(decoded&&decoded->sceneId==2&&decoded->frameId==60,"scaled exact frame code decoded");
        check(!srProofDecodeCode(codeRaster(scale,{0xD3A5,0x203C,0xEC37,0xDFC3}),1280*scale,720*scale),"bad CRC rejected");
        check(!srProofDecodeCode(codeRaster(scale,{0xD3A5,0x0000,0x1D0F,0xFFFF}),1280*scale,720*scale),"CRC-valid non-narrator scene rejected");
        check(!srProofDecodeCode(codeRaster(scale,{0xD3A4,0x203C,0xEC36,0xDFC3}),1280*scale,720*scale),"bad sync rejected");
        check(!srProofDecodeCode(codeRaster(scale,{0xD3A5,0x203C,0xEC36,0xDFC2}),1280*scale,720*scale),"bad inverse rejected");
        check(!srProofDecodeCode(codeRaster(scale,{0xD3A5,0x203C,0xEC36,0xDFC3},100,139),1280*scale,720*scale),"low contrast rejected");
        raster.pop_back();check(!srProofDecodeCode(raster,1280*scale,720*scale),"truncated RGBA rejected");
    }
    SrProofCounter filtered;filtered.sourceIds={26};
    for(uint64_t i=1;i<=25;++i)check(!filtered.candidate(i),"non-target file frames never read back");
    check(filtered.candidate(26),"matching local sequence admitted after warmup");
    SrProofCounter counter;counter.limit=3;
    for(uint64_t i=1;i<=24;++i){check(!counter.candidate(i),"warmup is not read back");check(!counter.candidate(i),"duplicate sequence ignored");}
    for(uint64_t i=25;i<=27;++i){check(counter.candidate(i),"three explicit real samples");counter.completed();}
    check(!counter.candidate(28),"hard maximum enforced");counter.failed=true;check(!counter.candidate(29),"failure disarms");
    check(srProofSdrByte(0)==0,"sRGB black");check(srProofSdrByte(1)==255,"sRGB white");
    check(srProofSdrByte(.0031308f)==10,"sRGB toe");check(srProofSdrByte(.18f)==118,"sRGB middle gray");
    check(srProofSdrByte(.5f)==188,"linear half gray not code half gray");
    check(srProofSdrByte(.5f,true)==128,"alpha is linear");
    check(srProofSdrByte(-.5f)==0&&srProofSdrByte(2)==255,"SDR clipping");
    check(srProofSdrByte(std::numeric_limits<float>::quiet_NaN())==0,"nonfinite fails black");
    for(unsigned i=0;i<256;++i){const double code=double(i)/255;const float linear=float(code<=.04045?code/12.92:std::pow((code+.055)/1.055,2.4));check(srProofSdrByte(linear)==i,"independent sRGB code roundtrip");}
    const auto& halfTables=srProofSdrHalfTables();
    for(unsigned bits=0;bits<65536;++bits){
        const float linear=DirectX::PackedVector::XMConvertHalfToFloat(uint16_t(bits));
        check(halfTables[0][bits]==srProofSdrByte(linear),"every FP16 color LUT byte equals original diagnostic conversion");
        check(halfTables[1][bits]==srProofSdrByte(linear,true),"every FP16 alpha LUT byte equals original diagnostic conversion");
    }
    if(handle!=INVALID_HANDLE_VALUE)std::filesystem::remove(fixture,error);
    std::cout<<"CHECKS="<<checks<<" FAILURES="<<failures<<" GPU_RUN=0\n";return failures?1:0;
}
