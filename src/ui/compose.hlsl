// AirGlass compositor: one full-window pass drawing the Liquid Glass slab, the video,
// the refractive bezel, the floating controls capsule and the connecting placeholder.
// Output is premultiplied alpha for a DirectComposition swap chain.

cbuffer ComposeCB : register(b0)
{
    float4 gView;       // w, h, 1/w, 1/h
    float4 gSlab;       // x0 y0 x1 y1 (window px)
    float4 gVideo;      // x0 y0 x1 y1 (picture rect)
    float4 gGeom;       // radius, bezel, dp (px per dip), fullscreen 0..1
    float4 gAppear;     // scale, opacity, y offset, time (s)
    float4 gShadow;     // offset y, softness, alpha, -
    float4 gVInfo;      // tex w, tex h, video alpha, reveal lod
    float4 gPill;       // capsule x0 y0 x1 y1
    float4 gPillInfo;   // alpha, ui scale, connecting amount, button radius
    float4 gDrop;       // hover droplet cx cy r alpha
    float4 gBtnX;       // button centre x (3), centre y
    float4 gBtnState;   // hover amount per button (3), press
    float4 gIcons;      // collapse icon 0/1, pinned 0/1, icon half size, stroke
    float4 gLabel;      // label rect x0 y0 x1 y1
    float4 gLabelInfo;  // label alpha, glyph cx, glyph cy, glyph half size
    float4 gTv0;        // 4th button x, 4th button hover, TV capsule 0/1, -
    float4 gTv1;        // TV corner (0 br, 1 bl, 2 tr, 3 tl), TV size (0 s, 1 m, 2 l), -, -
};

Texture2D<float4> tVideo : register(t0);
Texture2D<float4> tLabel : register(t1);
SamplerState sLin : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSFull(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// ------------------------------------------------------------------------------------------
// Signed distance helpers (pixels, negative inside)

float CornerLen4(float2 q)
{
    q = max(q, 0.0);
    float2 q2 = q * q;
    return sqrt(sqrt(q2.x * q2.x + q2.y * q2.y));
}

// Rounded rectangle with superellipse ("continuous") corners.
float sdSquircle(float2 p, float2 b, float r)
{
    r = min(r, min(b.x, b.y));
    float2 q = abs(p) - b + r;
    return CornerLen4(q) + min(max(q.x, q.y), 0.0) - r;
}

float sdRoundBox(float2 p, float2 b, float r)
{
    r = min(r, min(b.x, b.y));
    float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float sdBox(float2 p, float2 b)
{
    float2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

float sdSegment(float2 p, float2 a, float2 b)
{
    float2 pa = p - a, ba = b - a;
    float h = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h);
}

float sdTriangle(float2 p, float2 p0, float2 p1, float2 p2)
{
    float2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    float2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
    float2 pq0 = v0 - e0 * saturate(dot(v0, e0) / dot(e0, e0));
    float2 pq1 = v1 - e1 * saturate(dot(v1, e1) / dot(e1, e1));
    float2 pq2 = v2 - e2 * saturate(dot(v2, e2) / dot(e2, e2));
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    float2 d = min(min(float2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)),
                       float2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
                       float2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

float SMin(float a, float b, float k)
{
    float h = saturate(0.5 + 0.5 * (b - a) / k);
    return lerp(b, a, h) - k * h * (1.0 - h);
}

float AA() { return 1.0 / max(gAppear.x, 0.01); }

float2 SlabCenter() { return (gSlab.xy + gSlab.zw) * 0.5; }
float2 SlabHalf() { return (gSlab.zw - gSlab.xy) * 0.5; }

float SlabSdf(float2 p) { return sdSquircle(p - SlabCenter(), SlabHalf(), gGeom.x); }

float2 SlabNormal(float2 p)
{
    const float e = 0.75;
    float2 g = float2(SlabSdf(p + float2(e, 0.0)) - SlabSdf(p - float2(e, 0.0)),
                      SlabSdf(p + float2(0.0, e)) - SlabSdf(p - float2(0.0, e)));
    float l = length(g);
    return l > 1e-5 ? g / l : float2(0.0, -1.0);
}

// ------------------------------------------------------------------------------------------
// Content (video or animated placeholder)

float3 SampleVideoLod(float2 uv, float lod)
{
    return tVideo.SampleLevel(sLin, saturate(uv), lod).rgb;
}

// Smooth blur at roughly mip level `lod`: 3x3 tent of bilinear taps one level finer, which
// hides the blockiness of sampling a coarse mip directly.
float3 SampleBlur(float2 uv, float lod)
{
    float l = max(lod - 1.0, 0.0);
    float2 t = 1.5 * exp2(l) / max(gVInfo.xy, 1.0);
    float3 c = SampleVideoLod(uv, l) * 4.0;
    c += (SampleVideoLod(uv + float2(-t.x, 0.0), l) + SampleVideoLod(uv + float2(t.x, 0.0), l) +
          SampleVideoLod(uv + float2(0.0, -t.y), l) + SampleVideoLod(uv + float2(0.0, t.y), l)) * 2.0;
    c += SampleVideoLod(uv + float2(-t.x, -t.y), l) + SampleVideoLod(uv + float2(t.x, -t.y), l) +
         SampleVideoLod(uv + float2(-t.x, t.y), l) + SampleVideoLod(uv + float2(t.x, t.y), l);
    return c / 16.0;
}

float3 CatmullRom(float2 uv)
{
    float2 texSize = gVInfo.xy;
    float2 samplePos = uv * texSize;
    float2 tc1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - tc1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 off12 = w2 / w12;
    float2 tc0 = (tc1 - 1.0) / texSize;
    float2 tc3 = (tc1 + 2.0) / texSize;
    float2 tc12 = (tc1 + off12) / texSize;
    float3 r = 0.0;
    r += tVideo.SampleLevel(sLin, float2(tc0.x, tc0.y), 0).rgb * (w0.x * w0.y);
    r += tVideo.SampleLevel(sLin, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y);
    r += tVideo.SampleLevel(sLin, float2(tc3.x, tc0.y), 0).rgb * (w3.x * w0.y);
    r += tVideo.SampleLevel(sLin, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y);
    r += tVideo.SampleLevel(sLin, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y);
    r += tVideo.SampleLevel(sLin, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y);
    r += tVideo.SampleLevel(sLin, float2(tc0.x, tc3.y), 0).rgb * (w0.x * w3.y);
    r += tVideo.SampleLevel(sLin, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y);
    r += tVideo.SampleLevel(sLin, float2(tc3.x, tc3.y), 0).rgb * (w3.x * w3.y);
    return saturate(r);
}

float2 VideoUV(float2 p)
{
    float2 vs = max(gVideo.zw - gVideo.xy, 1.0);
    return (p - gVideo.xy) / vs;
}

float VideoRatio()
{
    float vw = max(gVideo.z - gVideo.x, 1.0);
    return gVInfo.x / vw;  // texels per screen pixel
}

float3 SampleVideoSharp(float2 p)
{
    float2 uv = VideoUV(p);
    float ratio = VideoRatio();
    float reveal = gVInfo.w;
    float3 c = float3(0.0, 0.0, 0.0);
    if (reveal > 0.01)
    {
        c = SampleBlur(uv, max(log2(max(ratio, 1.0)), 0.0) + reveal);
    }
    else if (ratio <= 1.0)
    {
        c = CatmullRom(uv);
    }
    else
    {
        // Shrinking: 2x2 taps one mip finer than the footprint (sharper than plain trilinear).
        float lod = max(log2(ratio) - 1.0, 0.0);
        float2 d = 0.25 / max(gVideo.zw - gVideo.xy, 1.0);
        c = (SampleVideoLod(uv + float2(-d.x, -d.y), lod) + SampleVideoLod(uv + float2(d.x, -d.y), lod) +
             SampleVideoLod(uv + float2(-d.x, d.y), lod) + SampleVideoLod(uv + float2(d.x, d.y), lod)) * 0.25;
    }
    return c;
}

float Blob(float2 q, float2 c, float r)
{
    float2 d = (q - c) / r;
    return exp(-dot(d, d) * 2.2);
}

float3 Aurora(float2 p)
{
    float2 vs = max(gVideo.zw - gVideo.xy, 1.0);
    float2 uv = (p - gVideo.xy) / vs;
    float t = gAppear.w;
    float asp = vs.x / vs.y;
    float2 q = float2(uv.x * asp, uv.y);
    float s = max(asp, 0.6);
    float3 c = float3(0.030, 0.035, 0.075);
    c += float3(0.16, 0.34, 0.98) * Blob(q, float2(asp * (0.28 + 0.10 * sin(t * 0.53)), 0.28 + 0.08 * cos(t * 0.61)), 0.55 * s);
    c += float3(0.56, 0.22, 0.92) * Blob(q, float2(asp * (0.78 + 0.10 * cos(t * 0.47)), 0.60 + 0.10 * sin(t * 0.39)), 0.60 * s);
    c += float3(0.04, 0.72, 0.76) * Blob(q, float2(asp * (0.35 + 0.12 * cos(t * 0.71)), 0.86 + 0.06 * sin(t * 0.83)), 0.45 * s);
    c += float3(0.98, 0.36, 0.56) * Blob(q, float2(asp * (0.70 + 0.09 * sin(t * 0.29)), 0.14 + 0.07 * cos(t * 0.57)), 0.40 * s);
    return c / (1.0 + c * 0.55);
}

// lod < 0: sharp sampling for the picture itself; otherwise blur levels above screen scale.
float3 Content(float2 p, float lod)
{
    float va = gVInfo.z;
    float3 ph = float3(0.0, 0.0, 0.0);
    if (va < 0.999)
        ph = Aurora(p);
    if (va <= 0.001)
        return ph;
    float3 v;
    if (lod < 0.0)
        v = SampleVideoSharp(p);
    else
        v = SampleBlur(VideoUV(p), max(log2(max(VideoRatio(), 1e-3)), 0.0) + lod + gVInfo.w);
    return lerp(ph, v, va);
}

// ------------------------------------------------------------------------------------------
// Glass

float3 GlassBezel(float2 p, float d)
{
    float dp = gGeom.z;
    float bezel = max(gGeom.y, 1.0);
    float2 n = SlabNormal(p);
    float depth = saturate(-d / bezel);  // 0 at the outer edge, 1 where the picture begins
    float wrap = 1.0 - depth;
    // Nearest point of the picture, then refract deeper inside: content bends round the rim.
    float2 q = clamp(p, gVideo.xy + 1.0, gVideo.zw - 1.0);
    float far = length(p - q);  // > bezel only in letterbox areas (morphs / full screen)
    float2 sp = q - n * (3.0 + 26.0 * wrap * wrap) * dp;
    float ca = 1.6 * dp * wrap;
    float lod = 2.5 + 2.0 * wrap + log2(1.0 + max(far - gGeom.y, 0.0) / (5.0 * dp));
    float3 c;
    c.r = Content(sp - n * ca, lod).r;
    c.g = Content(sp, lod).g;
    c.b = Content(sp + n * ca, lod).b;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(l, l, l), c, 1.3);
    c = c * 0.80 + 0.15;
    float slope = wrap * wrap;
    float3 N = normalize(float3(n * slope * 1.8, 1.0));
    float3 R = reflect(float3(0.0, 0.0, -1.0), N);
    c += pow(saturate(dot(R, normalize(float3(-0.5, -0.8, 0.45)))), 16.0) * 0.65;
    c += pow(saturate(dot(R, normalize(float3(0.5, 0.8, 0.45)))), 16.0) * 0.30;
    c += pow(wrap, 3.0) * 0.12;
    c *= 1.0 - 0.30 * saturate((far - gGeom.y) / (80.0 * dp));  // deep letterbox glass reads darker
    return saturate(c);
}

float3 SlabColor(float2 p, float d)
{
    float fs = gGeom.w;
    float2 vc = (gVideo.xy + gVideo.zw) * 0.5;
    float2 vb = (gVideo.zw - gVideo.xy) * 0.5;
    float innerR = max(gGeom.x - gGeom.y, 0.0);
    float dV = sdSquircle(p - vc, vb, innerR);
    float vCov = saturate(0.5 - dV / AA());
    float3 glass = float3(0.0, 0.0, 0.0);
    if (vCov < 1.0)
        glass = lerp(GlassBezel(p, d), float3(0.0, 0.0, 0.0), fs);
    float3 content = float3(0.0, 0.0, 0.0);
    if (vCov > 0.0)
        content = Content(p, -1.0);
    float3 c = lerp(glass, content, vCov);
    float edge = saturate(1.0 - abs(dV) / (1.5 * gGeom.z)) * (1.0 - fs) * step(0.0, dV);
    return c * (1.0 - 0.18 * edge);
}

float3 ApplyRim(float2 p, float d, float3 c)
{
    float fs = gGeom.w;
    if (fs >= 0.999)
        return c;
    float dp = gGeom.z;
    float2 n = SlabNormal(p);
    float band = saturate(1.0 - abs(d + 0.9 * dp) / (0.9 * dp));
    float2 ld = normalize(float2(-0.55, -0.83));
    float key = pow(saturate(dot(n, ld)), 1.5);
    float fill = pow(saturate(dot(n, -ld)), 1.5);
    float rim = band * (0.20 + 0.55 * key + 0.28 * fill) * (1.0 - fs);
    return lerp(c, float3(1.0, 1.0, 1.0), rim);
}

// ------------------------------------------------------------------------------------------
// Connecting placeholder: AirPlay glyph + device label

float AirPlayGlyph(float2 g)
{
    float screen = abs(sdRoundBox(g - float2(0.0, -0.18), float2(0.92, 0.62), 0.16)) - 0.075;
    float cut = sdBox(g - float2(0.0, 0.55), float2(0.40, 0.30));
    screen = max(screen, -cut);
    float tri = sdTriangle(g, float2(0.0, 0.16), float2(-0.46, 0.80), float2(0.46, 0.80)) - 0.04;
    return min(screen, tri);
}

float3 ApplyPlaceholder(float2 p, float3 c)
{
    float amt = gPillInfo.z;
    if (amt <= 0.001)
        return c;
    float gs = gLabelInfo.w;
    float2 g = (p - gLabelInfo.yz) / gs;
    float dg = AirPlayGlyph(g) * gs;
    float ga = saturate(0.5 - dg / AA());
    float pulse = 0.5 + 0.5 * sin(gAppear.w * 2.6);
    float glow = exp(-max(dg, 0.0) / (10.0 * gGeom.z)) * (0.10 + 0.12 * pulse);
    float3 o = c + glow * amt;
    o = lerp(o, float3(1.0, 1.0, 1.0), ga * 0.92 * amt);
    if (p.x >= gLabel.x && p.x < gLabel.z && p.y >= gLabel.y && p.y < gLabel.w)
    {
        float2 luv = (p - gLabel.xy) / max(gLabel.zw - gLabel.xy, 1.0);
        float4 t = tLabel.SampleLevel(sLin, luv, 0);
        float k = amt * gLabelInfo.x;
        o = o * (1.0 - t.a * k) + t.rgb * k;
    }
    return o;
}

// ------------------------------------------------------------------------------------------
// Controls capsule

float PillSdf(float2 p)
{
    float2 pc = (gPill.xy + gPill.zw) * 0.5;
    float2 pb = (gPill.zw - gPill.xy) * 0.5;
    float dP = sdRoundBox(p - pc, pb, pb.y);
    float dp = gGeom.z * gPillInfo.y;
    float dD = length(p - gDrop.xy) - gDrop.z + (1.0 - gDrop.w) * 30.0 * dp;
    return SMin(dP, dD, 9.0 * dp);
}

float IconClose(float2 q, float s)
{
    return min(sdSegment(q, float2(-s, -s), float2(s, s)), sdSegment(q, float2(-s, s), float2(s, -s)));
}

float IconFullscreen(float2 q, float s, float collapse)
{
    float2 a0 = float2(0.12 * s, -0.12 * s), a1 = float2(s, -s);
    float2 b0 = float2(-0.12 * s, 0.12 * s), b1 = float2(-s, s);
    float d = min(sdSegment(q, a0, a1), sdSegment(q, b0, b1));
    float h = 0.55 * s;
    float sgn = collapse > 0.5 ? -1.0 : 1.0;
    float2 ta = collapse > 0.5 ? a0 : a1;
    float2 tb = collapse > 0.5 ? b0 : b1;
    d = min(d, sdSegment(q, ta, ta + float2(-h, 0.0) * sgn));
    d = min(d, sdSegment(q, ta, ta + float2(0.0, h) * sgn));
    d = min(d, sdSegment(q, tb, tb + float2(h, 0.0) * sgn));
    d = min(d, sdSegment(q, tb, tb + float2(0.0, -h) * sgn));
    return d;
}

float IconPin(float2 q, float s, float filled)
{
    float2 r = float2(q.x + q.y, q.y - q.x) * 0.70710678;
    float head = sdRoundBox(r - float2(0.0, -0.62 * s), float2(0.44 * s, 0.14 * s), 0.07 * s);
    float body = sdBox(r - float2(0.0, -0.26 * s), float2(0.24 * s, 0.26 * s));
    float plate = sdRoundBox(r - float2(0.0, 0.04 * s), float2(0.52 * s, 0.07 * s), 0.05 * s);
    float needle = sdSegment(r, float2(0.0, 0.10 * s), float2(0.0, 0.98 * s));
    float shape = min(min(head, body), plate);
    return filled > 0.5 ? min(shape, needle) : min(abs(shape), needle);
}

float IconAlpha(float d, float stroke)
{
    return saturate(0.5 - (d - stroke * 0.5) / AA());
}

float FillAlpha(float d)
{
    return saturate(0.5 - d / AA());
}

// Screen outline with a small picture in the current corner ("move to corner").
float IconCorner(float2 q, float s, float stroke, float corner)
{
    float2 hb = float2(0.95, 0.72) * s;
    float frame = IconAlpha(abs(sdRoundBox(q, hb, 0.22 * s)), stroke);
    int c = (int)round(corner);
    float2 sg = float2((c == 1 || c == 3) ? -1.0 : 1.0, (c >= 2) ? -1.0 : 1.0);
    float2 hp = float2(0.36, 0.22) * s;
    float2 cpos = sg * max(hb - stroke * 0.5 - 0.14 * s - hp, 0.0);
    return max(frame, FillAlpha(sdRoundBox(q - cpos, hp, 0.08 * s)));
}

// Three rising bars, lit up to the current size.
float IconSize(float2 q, float s, float level)
{
    float a = 0.0;
    int lv = (int)round(level);
    [unroll] for (int i = 0; i < 3; ++i)
    {
        float hh = (0.30 + 0.22 * i) * s;
        float2 c = float2(float(i - 1) * 0.62 * s, 0.74 * s - hh);
        float d = sdRoundBox(q - c, float2(0.19 * s, hh), 0.08 * s);
        a = max(a, FillAlpha(d) * (i <= lv ? 1.0 : 0.35));
    }
    return a;
}

float Icons(float2 p)
{
    float cy = gBtnX.w;
    float s = gIcons.z;
    float stroke = gIcons.w;
    float press = gBtnState.w;
    float a = 0.0;
    float sc0 = 1.0 + 0.10 * gBtnState.x - 0.10 * press * gBtnState.x;
    float sc1 = 1.0 + 0.10 * gBtnState.y - 0.10 * press * gBtnState.y;
    float sc2 = 1.0 + 0.10 * gBtnState.z - 0.10 * press * gBtnState.z;
    float2 q0 = (p - float2(gBtnX.x, cy)) / sc0;
    float2 q1 = (p - float2(gBtnX.y, cy)) / sc1;
    float2 q2 = (p - float2(gBtnX.z, cy)) / sc2;
    if (gTv0.z > 0.5)
    {
        // TV Mode capsule: full screen, move corner, size, stop.
        float sc3 = 1.0 + 0.10 * gTv0.y - 0.10 * press * gTv0.y;
        float2 q3 = (p - float2(gTv0.x, cy)) / sc3;
        a = max(a, IconAlpha(IconFullscreen(q0, s * 0.86, gIcons.x), stroke));
        a = max(a, IconCorner(q1, s, stroke, gTv1.x));
        a = max(a, IconSize(q2, s, gTv1.y));
        a = max(a, FillAlpha(sdRoundBox(q3, float2(0.56, 0.56) * s, 0.14 * s)));
    }
    else
    {
        a = max(a, IconAlpha(IconClose(q0, s * 0.72), stroke));
        a = max(a, IconAlpha(IconPin(q1, s, gIcons.y), stroke));
        a = max(a, IconAlpha(IconFullscreen(q2, s * 0.86, gIcons.x), stroke));
    }
    return a;
}

float3 ApplyPill(float2 p, float3 c)
{
    float alpha = gPillInfo.x;
    if (alpha <= 0.002)
        return c;
    float dp = gGeom.z * gPillInfo.y;
    float dM = PillSdf(p);
    float dSh = PillSdf(p - float2(0.0, 3.0 * dp));
    float shade = (1.0 - smoothstep(-4.0 * dp, 16.0 * dp, dSh)) * 0.22 * alpha;
    c *= 1.0 - shade;
    float cov = saturate(0.5 - dM / AA()) * alpha;
    if (cov <= 0.0)
        return c;

    const float e = 0.75;
    float2 n = float2(PillSdf(p + float2(e, 0.0)) - PillSdf(p - float2(e, 0.0)),
                      PillSdf(p + float2(0.0, e)) - PillSdf(p - float2(0.0, e)));
    n = length(n) > 1e-5 ? normalize(n) : float2(0.0, -1.0);
    float2 pc = (gPill.xy + gPill.zw) * 0.5;
    float halfH = (gPill.w - gPill.y) * 0.5;
    float depth = saturate(-dM / (halfH * 0.9));
    float edge = 1.0 - depth;
    float inDrop = saturate(0.5 - (length(p - gDrop.xy) - gDrop.z) / AA()) * gDrop.w;
    float2 lc = lerp(pc, gDrop.xy, inDrop);
    float mag = lerp(0.90, 0.80, inDrop);
    float2 q = lc + (p - lc) * mag;
    q -= n * edge * edge * 9.0 * dp;
    float ca = edge * 1.4 * dp;
    float lod = 2.6 - 0.9 * inDrop;
    float3 bg;
    bg.r = Content(q - n * ca, lod).r;
    bg.g = Content(q, lod).g;
    bg.b = Content(q + n * ca, lod).b;
    float3 avg = Content(pc, 5.0);
    float lum = dot(avg, float3(0.2126, 0.7152, 0.0722));
    float dark = smoothstep(0.60, 0.40, lum);
    // Bright content: milky frosted glass with dark glyphs. Dark content: smoky glass, white glyphs.
    float3 glass = lerp(bg * 0.60 + 0.32, bg * 0.62 + 0.06, dark);
    glass += inDrop * lerp(0.08, 0.10, dark);
    float slope = edge * edge;
    float3 N = normalize(float3(n * slope * 1.6, 1.0));
    float3 R = reflect(float3(0.0, 0.0, -1.0), N);
    glass += pow(saturate(dot(R, normalize(float3(-0.5, -0.8, 0.45)))), 18.0) * 0.55;
    glass += pow(saturate(dot(R, normalize(float3(0.5, 0.8, 0.45)))), 18.0) * 0.22;
    float rimBand = saturate(1.0 - abs(dM + 0.8 * dp) / (0.8 * dp));
    float2 ld = normalize(float2(-0.55, -0.83));
    glass = lerp(glass, float3(1.0, 1.0, 1.0),
                 rimBand * (0.18 + 0.45 * pow(saturate(dot(n, ld)), 1.5) + 0.20 * pow(saturate(dot(n, -ld)), 1.5)));
    float3 iconCol = lerp(float3(0.10, 0.10, 0.12), float3(1.0, 1.0, 1.0), dark);
    glass = lerp(glass, iconCol, Icons(p));
    return lerp(c, saturate(glass), cov);
}

// ------------------------------------------------------------------------------------------

float4 PSCompose(VSOut i) : SV_Target
{
    float2 pix = i.pos.xy;
    float fs = gGeom.w;
    float opacity = gAppear.y;
    float2 sc = SlabCenter();
    float2 p = sc + (pix - sc - float2(0.0, gAppear.z)) / max(gAppear.x, 0.01);

    float backA = fs * opacity;
    float dS = sdSquircle(p - sc - float2(0.0, gShadow.x), SlabHalf(), gGeom.x);
    float sh = 1.0 - smoothstep(-gShadow.y, gShadow.y * 2.0, dS);
    float shadowA = sh * sh * gShadow.z * (1.0 - fs) * opacity;
    float4 outc = float4(0.0, 0.0, 0.0, max(backA, shadowA));

    float d = SlabSdf(p);
    float cov = saturate(0.5 - d / AA());
    if (cov <= 0.0)
        return outc;

    float3 col = SlabColor(p, d);
    col = ApplyPlaceholder(p, col);
    col = ApplyPill(p, col);
    col = ApplyRim(p, d, col);
    float a = cov * opacity;
    return float4(col * a, a) + outc * (1.0 - a);
}
