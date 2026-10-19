// GPUPixel basic beauty adaptation, Apache-2.0, PixPark 2021.
// Provenance and full license: third_party_notices/gpupixel-beauty.
float3 BeautyEncode(float3 c) {
    c=saturate(c);
    return select(c<=.0031308,c*12.92,1.055*pow(max(c,1e-8),1.0/2.4)-.055);
}
float3 BeautyDecode(float3 c) {
    c=saturate(c);
    return select(c<=.04045,c/12.92,pow((c+.055)/1.055,2.4));
}
float3 BeautyQ8(float3 c) {return round(saturate(c)*255.0)/255.0;}
float4 BeautySample(Texture2D<float4> tex,float2 uv) {
    uint w,h;tex.GetDimensions(w,h);
    float2 pos=uv*float2(w,h)-.5;
    int2 p=int2(floor(pos));float2 f=frac(pos);
    int2 limit=int2(w,h)-1;
    float4 a=tex.Load(int3(clamp(p,int2(0,0),limit),0));
    float4 b=tex.Load(int3(clamp(p+int2(1,0),int2(0,0),limit),0));
    float4 c=tex.Load(int3(clamp(p+int2(0,1),int2(0,0),limit),0));
    float4 d=tex.Load(int3(clamp(p+int2(1,1),int2(0,0),limit),0));
    return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
}
