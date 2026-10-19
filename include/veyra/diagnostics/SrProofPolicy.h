#pragma once
// Task-scoped, opt-in diagnostic authority only. No media or GPU operations.
#include <windows.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace veyra::diagnostics {
inline const std::filesystem::path& srProofTaskRoot() {
    static const std::filesystem::path root(L"E:/\u9879\u76ee/Veyra/browser-sr-stage22-20261018");
    return root;
}
inline std::wstring srProofFold(std::filesystem::path path) {
    auto text=path.lexically_normal().generic_wstring();
    std::transform(text.begin(),text.end(),text.begin(),[](wchar_t c){return std::towlower(c);});
    while(text.size()>3&&text.back()==L'/')text.pop_back();
    return text;
}
inline bool srProofPlainPath(const std::filesystem::path& path) {
    if(!path.is_absolute())return false;
    std::filesystem::path current;
    for(const auto& part:path){
        if(part==L".."||part==L".")return false;
        current/=part;
        const auto attr=GetFileAttributesW(current.c_str());
        if(attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    }
    return true;
}
inline bool srProofWithin(const std::filesystem::path& path,const std::filesystem::path& root) {
    if(!srProofPlainPath(path)||!srProofPlainPath(root))return false;
    std::error_code error;
    const auto actual=std::filesystem::weakly_canonical(path,error);if(error)return false;
    const auto parent=std::filesystem::weakly_canonical(root,error);if(error)return false;
    const auto a=srProofFold(actual),b=srProofFold(parent);
    return a.size()>b.size()&&a.compare(0,b.size(),b)==0&&a[b.size()]==L'/';
}
struct SrProofPolicy {
    std::filesystem::path directory,file;
    uint64_t window=0;unsigned frames=1;std::vector<uint64_t> sourceIds;std::vector<unsigned> codeIds;
    static std::optional<SrProofPolicy> parse(const std::wstring& directory,const std::wstring& target,const std::wstring& frames,const std::wstring& sourceIds=L"",const std::wstring& codeIds=L"") {
        SrProofPolicy p;p.directory=directory;
        if(!srProofWithin(p.directory,srProofTaskRoot()/L"tests"))return {};
        if(!frames.empty()){if(frames!=L"1"&&frames!=L"2"&&frames!=L"3")return {};p.frames=unsigned(frames[0]-L'0');}
        const std::wstring prefix=L"screen-window:";
        if(target.starts_with(prefix)){
            const auto digits=target.substr(prefix.size());if(digits.empty()||digits.size()>20)return {};
            for(wchar_t c:digits){if(c<L'0'||c>L'9'||p.window>(UINT64_MAX-uint64_t(c-L'0'))/10)return {};p.window=p.window*10+uint64_t(c-L'0');}
            if(!p.window)return {};
        }else{
            p.file=target;
            if(!srProofWithin(p.file,srProofTaskRoot()/L"fixtures"))return {};
            std::error_code error;if(!std::filesystem::is_regular_file(p.file,error)||error)return {};
        }
        if(!sourceIds.empty()){
            if(p.window)return {};
            uint64_t value=0;bool digit=false;
            for(size_t i=0;i<=sourceIds.size();++i){
                const wchar_t c=i==sourceIds.size()?L',':sourceIds[i];
                if(c==L','){
                    if(!digit||!value||p.sourceIds.size()>=3||(!p.sourceIds.empty()&&value<=p.sourceIds.back()))return {};
                    p.sourceIds.push_back(value);value=0;digit=false;
                }else{
                    if(c<L'0'||c>L'9'||value>(UINT64_MAX-uint64_t(c-L'0'))/10)return {};
                    value=value*10+uint64_t(c-L'0');digit=true;
                }
            }
            if(p.sourceIds.size()!=p.frames)return {};
        }
        if(!codeIds.empty()){
            if(!p.window||!sourceIds.empty())return {};
            unsigned value=0;bool digit=false;
            for(size_t i=0;i<=codeIds.size();++i){
                const wchar_t c=i==codeIds.size()?L',':codeIds[i];
                if(c==L','){
                    if(!digit||p.codeIds.size()>=3||(!p.codeIds.empty()&&value<=p.codeIds.back()))return {};
                    p.codeIds.push_back(value);value=0;digit=false;
                }else{
                    if(c<L'0'||c>L'9'||value>299/10)return {};
                    value=value*10+unsigned(c-L'0');if(value>299)return {};digit=true;
                }
            }
            if(p.codeIds.size()!=p.frames)return {};
        }
        return p;
    }
    bool allowsWindow(uint64_t actual,const std::wstring& title)const {
        return window&&window==actual&&title.starts_with(L"Veyra SR Proof");
    }
    bool allowsFile(const std::filesystem::path& actual)const {
        return !window&&srProofPlainPath(actual)&&srProofWithin(actual,srProofTaskRoot()/L"fixtures")&&srProofFold(actual)==srProofFold(file);
    }
};
struct SrProofCounter {
    static constexpr unsigned Warmup=24;
    unsigned observed=0,saved=0,limit=1;uint64_t last=0;bool failed=false;std::vector<uint64_t> sourceIds;
    bool candidate(uint64_t sequence) {
        if(failed||saved>=limit||!sequence||sequence==last)return false;
        last=sequence;++observed;
        return observed>Warmup&&(sourceIds.empty()||std::find(sourceIds.begin(),sourceIds.end(),sequence)!=sourceIds.end());
    }
    void completed(){++saved;}
    void reopened(){observed=0;last=0;}
};
template<class Settings> inline bool srProofSameRecipe(Settings a,Settings b) {
    a.revision=0;b.revision=0;return a==b;
}
inline bool srProofCanResume(const SrProofPolicy& oldPolicy,const std::wstring& oldSource,unsigned oldQuality,
                             const SrProofPolicy& next,const std::wstring& nextSource,unsigned nextQuality,
                             long owners,bool sameRecipe,bool failed) {
    return owners==1&&!failed&&sameRecipe&&oldPolicy.window&&next.window==oldPolicy.window&&
        !oldPolicy.codeIds.empty()&&next.codeIds==oldPolicy.codeIds&&next.frames==oldPolicy.frames&&
        next.sourceIds==oldPolicy.sourceIds&&srProofFold(oldPolicy.directory)==srProofFold(next.directory)&&
        oldSource==nextSource&&oldQuality==nextQuality;
}
// Stable diagnostic fingerprint; exact typed settings equality, not this hash,
// authorizes a resumed source. The report retains the canonical active recipe.
inline uint64_t srProofRecipeHash(std::string_view recipe) {
    uint64_t hash=14695981039346656037ull;
    for(unsigned char byte:recipe){hash^=byte;hash*=1099511628211ull;}
    return hash;
}
struct SrProofCode { unsigned sceneId=0,frameId=0; };
// Pure CPU decoder for the authorized narrator fixture, never an output repair.
inline std::optional<SrProofCode> srProofDecodeCode(const std::vector<uint8_t>& pixels,uint32_t width,uint32_t height) {
    if(width<1280||height<720||uint64_t(width)*720!=uint64_t(height)*1280||
       uint64_t(width)*height>SIZE_MAX/4||pixels.size()!=size_t(width)*height*4)return {};
    const double sx=double(width)/1280,sy=double(height)/720;
    std::array<std::array<double,16>,4> values{};
    const auto median=[](auto samples){std::sort(samples.begin(),samples.end());const auto n=samples.size();return n%2?samples[n/2]:(samples[n/2-1]+samples[n/2])*.5;};
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<16;++col){
        std::array<double,9> samples{};unsigned n=0;
        for(double fy:{.35,.5,.65})for(double fx:{.35,.5,.65}){
            const auto x=uint32_t(std::lround((8+4+(col+fx)*8)*sx));
            const auto y=uint32_t(std::lround((8+4+(row+fy)*8)*sy));
            if(x>=width||y>=height)return {};
            const auto at=(size_t(y)*width+x)*4;
            samples[n++]=.2126*pixels[at]+.7152*pixels[at+1]+.0722*pixels[at+2];
        }
        values[row][col]=median(samples);
    }
    constexpr uint16_t sync=0xD3A5;std::vector<double> blacks,whites;
    for(unsigned col=0;col<16;++col)((sync>>(15-col))&1?whites:blacks).push_back(values[0][col]);
    const double black=median(blacks),white=median(whites),threshold=(white+black)*.5;
    if(white-black<40)return {};
    std::array<uint16_t,4> words{};
    for(unsigned row=0;row<4;++row)for(double value:values[row])words[row]=uint16_t((words[row]<<1)|(value>threshold?1:0));
    const uint16_t payload=words[1];uint16_t crc=0xFFFF;
    for(unsigned byte:{unsigned(payload>>8),unsigned(payload&255)}){
        crc^=uint16_t(byte<<8);
        for(unsigned bit=0;bit<8;++bit)crc=uint16_t((crc<<1)^((crc&0x8000)?0x1021:0));
    }
    const unsigned scene=payload>>12,frame=payload&0xFFF;
    if(words[0]!=sync||words[2]!=crc||words[3]!=uint16_t(payload^0xFFFF)||scene!=2||frame>=300)return {};
    return SrProofCode{scene,frame};
}
// Same SDR OETF as PresentBlit.hlsl. Alpha is linear coverage, not colour.
inline uint8_t srProofSdrByte(float value,bool alpha=false) {
    if(!std::isfinite(value))return 0;
    const double linear=std::clamp(double(value),0.0,1.0);
    const double encoded=alpha?linear:linear<=0.0031308?12.92*linear:1.055*std::pow(linear,1.0/2.4)-0.055;
    return uint8_t(std::lround(std::clamp(encoded,0.0,1.0)*255.0));
}
// Diagnostic FP16 readback only: cache the unchanged scalar conversion for all
// bit patterns, including subnormals, infinities and NaNs. Index 1 is alpha.
inline const std::array<std::array<uint8_t,65536>,2>& srProofSdrHalfTables() {
    static const auto tables=[] {
        std::array<std::array<uint8_t,65536>,2> result{};
        for(unsigned bits=0;bits<65536;++bits){
            const float linear=DirectX::PackedVector::XMConvertHalfToFloat(uint16_t(bits));
            result[0][bits]=srProofSdrByte(linear);
            result[1][bits]=srProofSdrByte(linear,true);
        }
        return result;
    }();
    return tables;
}
}
