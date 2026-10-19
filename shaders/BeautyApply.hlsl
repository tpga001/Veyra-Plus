// GPUPixel BeautyFaceUnit + BoxDifference (delta=7.07), Apache-2.0.
// Source alpha is preserved. Whitening retains the upstream four-LUT full
// endpoint; Veyra strength continuously blends the whole sequence from input.
// No semantic skin/face mask; the smoothing p term is a red-channel heuristic.
#include "BeautyCommon.hlsli"
cbuffer Params:register(b0) {uint width,height;float smoothing,whitening;float detail;float3 unused;};
Texture2D<float4> source:register(t0);
Texture2D<float4> meanTexture:register(t1);
Texture2D<float4> grayLut:register(t2);
Texture2D<float4> originLut:register(t3);
Texture2D<float4> skinLut:register(t4);
Texture2D<float4> lightLut:register(t5);
RWTexture2D<float4> target:register(u0);
float3 Encoded(int2 p) {return BeautyEncode(source.Load(int3(clamp(p,int2(0,0),int2(width,height)-1),0)).rgb);}
float3 Lut16(Texture2D<float4> lut,float3 c) {
    c=saturate(c);float blue=c.b*15;
    float2 q1=float2(floor(blue)-floor(floor(blue)*.25)*4,floor(floor(blue)*.25));
    float2 q2=float2(ceil(blue)-floor(ceil(blue)*.25)*4,floor(ceil(blue)*.25));
    float2 offset=c.rg*.234375+.0078125;
    return lerp(BeautySample(lut,q1*.25+offset).rgb,BeautySample(lut,q2*.25+offset).rgb,frac(blue));
}
float3 WhitenFull(float3 c) {
    const float alpha=.7;
    float3 epm=c;
    c=saturate((epm-.0258820)*1.02657);
    float3 texel=float3(BeautySample(grayLut,float2(c.r,.5)).r,
                       BeautySample(grayLut,float2(c.g,.5)).g,
                       BeautySample(grayLut,float2(c.b,.5)).b);
    texel=lerp(epm,lerp(c,texel,.5),alpha);
    texel=lerp(Lut16(originLut,texel),c,alpha);
    c=saturate(Lut16(skinLut,texel));
    float blue=c.b*63;
    float2 q1=float2(floor(blue)-floor(floor(blue)/8)*8,floor(floor(blue)/8));
    float2 q2=float2(ceil(blue)-floor(ceil(blue)/8)*8,floor(ceil(blue)/8));
    float2 offset=.5/512+(1.0/8-1.0/512)*c.rg;
    float3 custom=lerp(BeautySample(lightLut,q1/8+offset).rgb,BeautySample(lightLut,q2/8+offset).rgb,frac(blue));
    return custom; // original full-strength endpoint; no partial pregrade jump
}
[numthreads(8,8,1)]
void main(uint3 p:SV_DispatchThreadID) {
    if(p.x>=width||p.y>=height)return;
    float4 baseLinear=source.Load(int3(p.xy,0));
    float3 color=BeautyEncode(baseLinear.rgb);
    if(smoothing>0) {
        float3 mean=meanTexture.Load(int3(p.xy,0)).rgb;
        float3 diff=(color-mean)*7.07;
        float3 variance=BeautyQ8(min(diff*diff,1));
        float meanVar=(variance.r+variance.g+variance.b)/3;
        float skinCandidate=saturate((min(color.r,mean.r-.1)-.2)*4);
        float k=saturate((1-meanVar/(meanVar+.1))*skinCandidate*smoothing);
        color=lerp(color,mean,k);
    }
    if(detail>0) {
        int2 xy=int2(p.xy);
        float3 base=Encoded(xy),sum=base*.25;
        sum+=(Encoded(xy+int2(1,0))+Encoded(xy-int2(1,0))+Encoded(xy+int2(0,1))+Encoded(xy-int2(0,1)))*.125;
        sum+=(Encoded(xy+int2(1,1))+Encoded(xy-int2(1,1))+Encoded(xy+int2(-1,1))+Encoded(xy+int2(1,-1)))*.0625;
        color+=(base-sum)*detail*2;
    }
    [branch] if(whitening>0) {
        // The upstream control only mixed the final light LUT, leaving gray,
        // origin and skin transforms fully active at any positive strength.
        // Blend the complete result instead, so 0+ approaches the input.
        float3 fullWhitening=WhitenFull(color);
        color=whitening>=1?fullWhitening:lerp(color,fullWhitening,whitening);
    }
    target[p.xy]=float4(BeautyDecode(color),baseLinear.a);
}
