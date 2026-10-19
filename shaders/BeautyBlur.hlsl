// Separable GPUPixel BoxMonoBlur radius 4, texel spacing 4.
// The optimized +/-1.5 and +/-3.5 offsets are bilinear reads, NOT nine
// integer reads with stride 4. Model the original RGBA8 intermediate.
#include "BeautyCommon.hlsli"
cbuffer Params:register(b0) {uint width,height,vertical,pad0;float4 unused;};
Texture2D<float4> source:register(t0);
RWTexture2D<float4> target:register(u0);
float3 SampleColor(float2 uv) {
    if(vertical)return BeautySample(source,uv).rgb;
    // GPU bilinear filtering happened in encoded color in the original GL
    // source. Encode the four linear texels before interpolating.
    float2 pos=uv*float2(width,height)-.5;
    int2 p=int2(floor(pos));float2 f=frac(pos);int2 lim=int2(width,height)-1;
    float3 a=BeautyEncode(source.Load(int3(clamp(p,int2(0,0),lim),0)).rgb);
    float3 b=BeautyEncode(source.Load(int3(clamp(p+int2(1,0),int2(0,0),lim),0)).rgb);
    float3 c=BeautyEncode(source.Load(int3(clamp(p+int2(0,1),int2(0,0),lim),0)).rgb);
    float3 d=BeautyEncode(source.Load(int3(clamp(p+int2(1,1),int2(0,0),lim),0)).rgb);
    return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
}
[numthreads(8,8,1)]
void main(uint3 p:SV_DispatchThreadID) {
    if(p.x>=width||p.y>=height)return;
    float2 uv=(float2(p.xy)+.5)/float2(width,height);
    float2 step=vertical?float2(0,4.0/height):float2(4.0/width,0);
    float3 mean=SampleColor(uv)/9;
    mean+=(SampleColor(uv+step*1.5)+SampleColor(uv-step*1.5))*2.0/9;
    mean+=(SampleColor(uv+step*3.5)+SampleColor(uv-step*3.5))*2.0/9;
    target[p.xy]=float4(BeautyQ8(mean),1);
}
