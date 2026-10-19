"""Independent scalar GPUPixel reference for Veyra synthetic beauty GPU tests.
Uses pinned local tracked PNGs and original GLSL equations, never video pixels.
Veyra changes modeled: CPU FP16 ingress RNE, GPU FP16 typed stores RTZ, Q8 blur/variance, input alpha,
independent detail control and continuous whole-pipeline whitening gain.
The full whitening endpoint remains the upstream four-LUT result.
No DLL/pixel-equivalence quality claim.
"""
import argparse, math, struct
from pathlib import Path
from PIL import Image
def half(v):return struct.unpack("<e",struct.pack("<e",v))[0]
def half_store(v):
    # D3D high-to-low precision FLOAT resource conversion rounds toward zero,
    # unlike the CPU fixture upload's struct.pack half nearest-even rounding.
    # Rule: https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#3.2.2
    v=struct.unpack("<f",struct.pack("<f",v))[0]
    rounded=half(v)
    if abs(rounded)>abs(v):
        bits=struct.unpack("<H",struct.pack("<e",rounded))[0]-1
        rounded=struct.unpack("<e",struct.pack("<H",bits))[0]
    return rounded
def sat(v):return min(1,max(0,v))
def encode(v):
    v=sat(v);return v*12.92 if v<=.0031308 else 1.055*v**(1/2.4)-.055
def decode(v):
    v=sat(v);return v/12.92 if v<=.04045 else ((v+.055)/1.055)**2.4
def q8(v):return math.floor(sat(v)*255+.5)/255
def mix(a,b,t):return [x+(y-x)*t for x,y in zip(a,b)]
def sample(image,u,v):
    w,h,pixels=image
    x,y=u*w-.5,v*h-.5;ix,iy=math.floor(x),math.floor(y);fx,fy=x-ix,y-iy
    def get(xx,yy):return pixels[min(h-1,max(0,yy))*w+min(w-1,max(0,xx))]
    return mix(mix(get(ix,iy),get(ix+1,iy),fx),mix(get(ix,iy+1),get(ix+1,iy+1),fx),fy)
def lut16(image,c):
    c=list(map(sat,c));blue=c[2]*15
    a,b=math.floor(blue),math.ceil(blue)
    p=[c[0]*.234375+.0078125,c[1]*.234375+.0078125]
    return mix(sample(image,(a%4)*.25+p[0],(a//4)*.25+p[1]),sample(image,(b%4)*.25+p[0],(b//4)*.25+p[1]),blue-a)[:3]
def main():
    ap=argparse.ArgumentParser();ap.add_argument("--gpupixel",type=Path,required=True);ap.add_argument("--output",type=Path,required=True);args=ap.parse_args()
    luts=[]
    for name in ("gray","origin","skin","light"):
        im=Image.open(args.gpupixel/"src"/"res"/("lookup_"+name+".png")).convert("RGBA")
        luts.append((im.width,im.height,[[c/255 for c in p] for p in im.getdata()]))
    w,h=32,24
    input=[]
    for y in range(h):
        for x in range(w):
            # skin-like warm patches, black/white, primary colors and detail edges.
            if x<16:
                c=[.57+((x*7+y*3)%9-4)/150,.36+((x*3+y*5)%7-3)/150,.28+((x+y)%5-2)/150]
            else:
                c=[(x-16)/15,y/23,((x*11+y*3)%24)/23]
            input.append([half(decode(v)) for v in c]+[half(.25+.75*((x+y)%4)/3)])
    encoded=[[encode(v) for v in p[:3]] for p in input]
    def get(arr,x,y):return arr[min(h-1,max(0,y))*w+min(w-1,max(0,x))]
    horizontal=[]
    for y in range(h):
        for x in range(w):
            horizontal.append([half_store(q8((get(encoded,x,y)[c]+2*sum(get(encoded,x+dx,y)[c] for dx in (6,-6,14,-14)))/9)) for c in range(3)])
    mean=[]
    for y in range(h):
        for x in range(w):
            mean.append([half_store(q8((get(horizontal,x,y)[c]+2*sum(get(horizontal,x,y+dy)[c] for dy in (6,-6,14,-14)))/9)) for c in range(3)])
    def whiten(color,strength):
        base=color[:]
        epm=color
        color=[sat((v-.0258820)*1.02657) for v in color]
        gray=[sample(luts[0],color[c],.5)[c] for c in range(3)]
        texel=mix(epm,mix(color,gray,.5),.7)
        texel=mix(lut16(luts[1],texel),color,.7)
        color=list(map(sat,lut16(luts[2],texel)))
        blue=color[2]*63;a,b=math.floor(blue),math.ceil(blue)
        p=[.5/512+(1/8-1/512)*color[0],.5/512+(1/8-1/512)*color[1]]
        custom=mix(sample(luts[3],(a%8)/8+p[0],(a//8)/8+p[1]),sample(luts[3],(b%8)/8+p[0],(b//8)/8+p[1]),blue-a)[:3]
        return custom if strength>=1 else mix(base,custom,strength)
    out=["// Generated independent scalar oracle; regenerate with generate-beauty-golden.py."]
    def emit(name,pixels):
        out.append("static constexpr float "+name+"[] = {")
        for p in pixels:out.append(",".join("%.9g"%v+"f" if "." in "%.9g"%v or "e" in "%.9g"%v else "%.9g"%v+".0f" for v in p)+",")
        out.append("};")
    emit("kBeautyInput",input)
    emit("kBeautyHorizontal",[p+[1.0] for p in horizontal])
    emit("kBeautyMean",[p+[1.0] for p in mean])
    for name,smooth,white,detail in (("Smooth",1,0,0),("Whiten",0,1,0),("Detail",0,0,1),("Mixed",.65,.35,.1),
                                      ("Whiten001Percent",0,.0001,0),("Whiten01Percent",0,.001,0),("Whiten1Percent",0,.01,0)):
        pixels=[]
        for y in range(h):
            for x in range(w):
                i=y*w+x;base=encoded[i];color=base[:]
                if smooth:
                    variance=[q8(min(((base[c]-mean[i][c])*7.07)**2,1)) for c in range(3)]
                    mean_var=sum(variance)/3;p=sat((min(base[0],mean[i][0]-.1)-.2)*4)
                    k=sat((1-mean_var/(mean_var+.1))*p*smooth);color=mix(color,mean[i],k)
                if detail:
                    low=[base[c]*.25+sum(get(encoded,x+dx,y+dy)[c] for dx,dy in ((1,0),(-1,0),(0,1),(0,-1)))*.125+sum(get(encoded,x+dx,y+dy)[c] for dx,dy in ((1,1),(-1,-1),(-1,1),(1,-1)))*.0625 for c in range(3)]
                    color=[color[c]+(base[c]-low[c])*detail*2 for c in range(3)]
                if white:color=whiten(color,white)
                pixels.append([half_store(decode(v)) for v in color]+[input[i][3]])
        emit("kBeauty"+name,pixels)
    args.output.write_text("\n".join(out)+"\n",encoding="utf-8")
if __name__=="__main__":main()
