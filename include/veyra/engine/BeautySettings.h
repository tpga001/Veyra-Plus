#pragma once
#include <cmath>
namespace veyra::engine {
// GPUPixel basic beauty shader port, SDR only. No detector/model or semantic
// face/skin mask. Values are uniforms; only enabled changes graph topology.
struct BeautySettings {
    bool enabled=false;
    float smoothing=.35f,whitening=.10f,detail=0.f;
    bool operator==(const BeautySettings&) const = default;
    bool valid() const {
        for(float v:{smoothing,whitening,detail})
            if(!std::isfinite(v)||v<0||v>1)return false;
        return true;
    }
    bool hasEffect() const {return enabled&&(smoothing>0||whitening>0||detail>0);}
    bool supported(bool hdrInput,bool hdrWorking=false) const {return !hdrInput&&!hdrWorking;}
};
}
