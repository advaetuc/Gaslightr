// =============================================================================
// Gaslightr — EnhanceCS.hlsl
// Phase 8A/8B: Direct3D 12 Compute Shader for Non-Neural Local Enhancement
//
// 7x7 box high-pass structure filter with controllable NR intensity blend,
// structure scale, and alpha channel preservation.
// =============================================================================

cbuffer FilterParams : register(b0)
{
    uint  g_Width;
    uint  g_Height;
    float g_Intensity;
    float g_Structure;
};

Texture2D<float4>   g_InputTex  : register(t0);
RWTexture2D<float4> g_OutputTex : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint x = dispatchThreadId.x;
    uint y = dispatchThreadId.y;

    if (x >= g_Width || y >= g_Height)
        return;

    int x0 = max(0, int(x) - 3);
    int x1 = min(int(g_Width), int(x) + 4);
    int y0 = max(0, int(y) - 3);
    int y1 = min(int(g_Height), int(y) + 4);

    float sum = 0.0f;
    for (int py = y0; py < y1; ++py)
    {
        for (int px = x0; px < x1; ++px)
        {
            float4 sample = g_InputTex[int2(px, py)];
            float lum = 0.2126729f * sample.r + 0.7151522f * sample.g + 0.0721750f * sample.b;
            sum += lum;
        }
    }

    float area = float((x1 - x0) * (y1 - y0));
    float mean = (area > 0.0f) ? (sum / area) : 0.0f;

    float4 center = g_InputTex[int2(x, y)];
    float centerLum = 0.2126729f * center.r + 0.7151522f * center.g + 0.0721750f * center.b;
    float detail = centerLum - mean;

    float4 result;
    result.rgb = center.rgb + g_Intensity * (g_Structure * detail);
    result.a   = center.a;

    g_OutputTex[int2(x, y)] = result;
}
