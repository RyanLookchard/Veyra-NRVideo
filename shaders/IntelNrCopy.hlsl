// Experimental Intel XMX DLSS-NR worker: a vendored copy of the upstream
// protocol-3 ENCODE/COMPOSE compute passes (dlss-nr-on-intel, Apache-2.0,
// commit 921d9754), reduced to the SDR RGBA8 path Veyra's parity proxy feeds.
// ENCODE: sRGB UNORM texture -> shared input buffer bytes (display-referred).
// COMPOSE: shared output buffer bytes -> linear-corrected final texture,
// guarded by the daemon's frame stamp; no stamp = keep the original frame.
#include "HdrColor.hlsli"

cbuffer IntelNrParams : register(b0)
{
    uint width;
    uint height;
    uint pitch;      // bytes per row of the shared buffers (width*4 here)
    uint stampLow;   // expected stamp frame value (low 32)
    uint stampHigh;  // expected stamp frame value (high 32)
    float intensity; // transfer strength, mixes the daemon's answer
    uint2 pad;
};

Texture2D<unorm float4> proxyTex : register(t0);
RWByteAddressBuffer inputBuf : register(u0);
ByteAddressBuffer outputBuf : register(t1);
Texture2D<float4> originalTex : register(t2);
RWTexture2D<float4> finalTex : register(u1);

float SrgbDecode1(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float3 SrgbDecode(float3 c)
{
    return float3(SrgbDecode1(c.r), SrgbDecode1(c.g), SrgbDecode1(c.b));
}

#if defined(ENCODE)
[numthreads(16,16,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    const float4 s = proxyTex[id.xy];
    // Upstream wire order is RGBA bytes (VkFormat 37 R8G8B8A8_UNORM).
    const uint r = uint(s.r * 255.0 + 0.5);
    const uint g = uint(s.g * 255.0 + 0.5);
    const uint b = uint(s.b * 255.0 + 0.5);
    const uint a = uint(s.a * 255.0 + 0.5);
    inputBuf.Store(id.y * pitch + id.x * 4, r | (g << 8) | (b << 16) | (a << 24));
}
#elif defined(COMPOSE)
[numthreads(16,16,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    const float4 original = originalTex[id.xy];
    // The stamp sits after the frame bytes; magic 'NRST' names this frame only.
    const uint stampBase = height * pitch;
    const uint magic = outputBuf.Load(stampBase);
    const uint lo = outputBuf.Load(stampBase + 4);
    const uint hi = outputBuf.Load(stampBase + 8);
    if (magic != 0x5453524Eu || lo != stampLow || hi != stampHigh)
    {
        finalTex[id.xy] = original;
        return;
    }
    const uint packed = outputBuf.Load(id.y * pitch + id.x * 4);
    const float3 neural = SrgbDecode(float3(
        float(packed & 255u) / 255.0,
        float((packed >> 8) & 255u) / 255.0,
        float((packed >> 16) & 255u) / 255.0));
    // The proxy already soft-shouldered the linear frame; the daemon answers
    // in the same display space. Decode both and mix in linear light, then
    // restore the original HDR base's excess (same rule as the AMD parity).
    const float3 proxyLinear = max(original.rgb, 0.0);
    const float3 shouldered = 0.75 + 0.25 * (1.0 - exp(-5.7780 * (proxyLinear - 0.75)));
    const float3 mapped = select(proxyLinear <= 0.75, proxyLinear, shouldered);
    const float3 answer = neural + max(0.0, proxyLinear - mapped);
    const float3 result = lerp(proxyLinear, answer, intensity);
    finalTex[id.xy] = float4(result, original.a);
}
#endif