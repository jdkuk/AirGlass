// YUV -> RGB conversion of decoded video frames (NV12 or planar 4:2:0).

cbuffer ConvertCB : register(b0)
{
    float4 cRow0;   // R = dot(row.xyz, yuv) + row.w
    float4 cRow1;
    float4 cRow2;
    float4 cInfo;   // x: 0 = NV12 (Y + interleaved UV), 1 = planar (Y, U, V)
};

Texture2D<float>  tY  : register(t0);
Texture2D<float2> tUV : register(t1);
Texture2D<float>  tU  : register(t2);
Texture2D<float>  tV  : register(t3);
SamplerState sConv : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 PSConvert(VSOut i) : SV_Target
{
    float y = tY.SampleLevel(sConv, i.uv, 0);
    float2 c;
    if (cInfo.x < 0.5)
        c = tUV.SampleLevel(sConv, i.uv, 0);
    else
        c = float2(tU.SampleLevel(sConv, i.uv, 0), tV.SampleLevel(sConv, i.uv, 0));
    float3 yuv = float3(y, c);
    float3 rgb = float3(dot(cRow0.xyz, yuv) + cRow0.w,
                        dot(cRow1.xyz, yuv) + cRow1.w,
                        dot(cRow2.xyz, yuv) + cRow2.w);
    return float4(saturate(rgb), 1.0);
}
