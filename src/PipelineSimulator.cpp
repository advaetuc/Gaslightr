// =============================================================================
// Gaslightr — PipelineSimulator.cpp
// Temporal Spoofing Engine — Phase 3 Implementation
//
// This is the core algorithm that "gaslights" the DLSS 5 neural network.
//
// References:
//   • photoshop-dlssnr-blueprint.md §Component C
//       — Halton sequence generator (exact code)
//       — Warmup loop structure (24 frames, ResetHistory on frame 0)
//       — Jitter normalisation: jitterX / srcWidth, jitterY / srcHeight
//   • dlss5_photoshop_blueprint.md §Phase 5
//       — Inverse motion vectors: "exact mathematical inverse of the jitter"
//       — Multi-pass ping-pong: "output of Pass N → input for Pass N+1"
//   • Photoshop DLSS 5 Plugin Architecture.md §Adapting Neural Pipeline
//       — Depth buffer at 1.0f (far plane, no spatial depth in 2D)
//       — Motion vectors "perfectly counteract applied sub-pixel jitter"
//       — Exposure texture: 1×1 at 1.0f for SDR
//       — Multi-pass: 1–4 passes (NODIX-TECH convention)
//
// Pipeline flow:
//
//   ┌───────────────────────────────────────────────────────────┐
//   │  TEMPORAL WARMUP  (24–32 frames)                         │
//   │                                                           │
//   │  for frame in 0..warmupFrames:                           │
//   │    jitterX = Halton(frame+1, 2) - 0.5                   │
//   │    jitterY = Halton(frame+1, 3) - 0.5                   │
//   │    motionVectors = (-jitterX, -jitterY)   ← INVERSE     │
//   │    EvaluateNR(input, depth, mv, output)                  │
//   │    WaitForGPU()                                          │
//   └───────────────────────────────────────────────────────────┘
//                            │
//                            ▼
//   ┌───────────────────────────────────────────────────────────┐
//   │  MULTI-PASS ENHANCEMENT  (1–4 passes)                    │
//   │                                                           │
//   │  for pass in 0..enhancePasses:                            │
//   │    input = (pass==0) ? warmup_output : previous_output   │
//   │    jitter = Halton(warmupFrames + pass + 1, base)        │
//   │    EvaluateNR(input → ping/pong)                         │
//   │    WaitForGPU()                                          │
//   │    swap(ping, pong)                                      │
//   └───────────────────────────────────────────────────────────┘
//
// =============================================================================

#include "PipelineSimulator.h"
#include "BackendCapabilities.h"
#include "shaders/EnhanceCS_Bytecode.h"

#include <cassert>
#include <cstring>   // memset, memcpy
#include <cstdio>
#include <vector>

namespace Gaslightr {

// ── Helper: bytes per pixel for a DXGI format ────────────────────────────────
static uint32_t BytesPerPixelFor(DXGI_FORMAT fmt) noexcept
{
    switch (fmt) {
    case DXGI_FORMAT_R32_FLOAT:            return 4;
    case DXGI_FORMAT_R16G16_FLOAT:         return 4;
    case DXGI_FORMAT_R16_FLOAT:            return 2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:   return 8;
    default:                               return 0;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ═════════════════════════════════════════════════════════════════════════════
PipelineSimulator::PipelineSimulator(DX12Context& ctx)
    : m_ctx(ctx)
{
}

PipelineSimulator::~PipelineSimulator()
{
    ReleaseSyntheticResources();
}

// ═════════════════════════════════════════════════════════════════════════════
// GetHaltonValue
// ═════════════════════════════════════════════════════════════════════════════
//
//  Exact implementation from photoshop-dlssnr-blueprint.md §Component C:
//
//    float GetHaltonValue(int index, int base) {
//        float result = 0.0f;
//        float f = 1.0f / base;
//        int i = index;
//        while (i > 0) {
//            result += f * (i % base);
//            f /= base;
//            i /= base;
//        }
//        return result;
//    }
//
//  Generates values in [0, 1) for the radical-inverse Halton sequence.
//  Base 2 gives: 0.5, 0.25, 0.75, 0.125, 0.625, 0.375, 0.875, ...
//  Base 3 gives: 0.333, 0.666, 0.111, 0.444, 0.777, 0.222, ...
//
float PipelineSimulator::GetHaltonValue(int index, int base) noexcept
{
    float result = 0.0f;
    float f = 1.0f / static_cast<float>(base);
    int i = index;
    while (i > 0) {
        result += f * static_cast<float>(i % base);
        f /= static_cast<float>(base);
        i /= base;
    }
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// TransitionBarrier  (static helper)
// ═════════════════════════════════════════════════════════════════════════════
void PipelineSimulator::TransitionBarrier(
    ID3D12GraphicsCommandList* pCmdList,
    ID3D12Resource*            pResource,
    D3D12_RESOURCE_STATES      before,
    D3D12_RESOURCE_STATES      after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = pResource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    pCmdList->ResourceBarrier(1, &barrier);
}

// ═════════════════════════════════════════════════════════════════════════════
// CreateDefaultTexture  (internal helper)
// ═════════════════════════════════════════════════════════════════════════════
DX12Result PipelineSimulator::CreateDefaultTexture(
    uint32_t width, uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_STATES initialState,
    Microsoft::WRL::ComPtr<ID3D12Resource>& outResource)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width              = width;
    texDesc.Height             = height;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = format;
    texDesc.SampleDesc.Count   = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags              = format == DXGI_FORMAT_R16G16B16A16_FLOAT
        ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = m_ctx.GetDevice()->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &texDesc, initialState,
        nullptr, IID_PPV_ARGS(&outResource));

    if (FAILED(hr)) {
        return DX12Result::Fail(hr, L"CreateDefaultTexture failed.");
    }
    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// CreateAndFillTexture  (internal helper)
// ═════════════════════════════════════════════════════════════════════════════
//
//  Creates a DEFAULT texture + UPLOAD staging buffer, fills the staging
//  buffer with the provided CPU data, records CopyTextureRegion on a fresh
//  command list, executes it, and waits for the GPU.
//
DX12Result PipelineSimulator::CreateAndFillTexture(
    uint32_t width, uint32_t height,
    DXGI_FORMAT format,
    const void* pInitData,
    uint32_t    srcRowPitch,
    Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture,
    Microsoft::WRL::ComPtr<ID3D12Resource>& outUpload)
{
    const uint32_t bpp = BytesPerPixelFor(format);
    if (bpp == 0) {
        return DX12Result::Fail(E_INVALIDARG, L"Unsupported DXGI_FORMAT.");
    }

    // ── Aligned pitch for the upload buffer ──────────────────────────────
    const uint32_t alignedPitch =
        MemoryBridge::CalculateAlignedPitch(width, bpp);
    const uint64_t bufferSize =
        static_cast<uint64_t>(alignedPitch) * height;

    // ── Create the DEFAULT texture ───────────────────────────────────────
    auto result = CreateDefaultTexture(
        width, height, format,
        D3D12_RESOURCE_STATE_COPY_DEST, outTexture);
    if (!result) return result;

    // ── Create the UPLOAD buffer ─────────────────────────────────────────
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width            = bufferSize;
        bufDesc.Height           = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels        = 1;
        bufDesc.Format           = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = m_ctx.GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&outUpload));
        if (FAILED(hr)) {
            return DX12Result::Fail(hr, L"UPLOAD buffer creation failed.");
        }
    }

    // ── Map + copy CPU data into the UPLOAD buffer (row-by-row) ──────────
    {
        void* pMapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        HRESULT hr = outUpload->Map(0, &readRange, &pMapped);
        if (FAILED(hr)) {
            return DX12Result::Fail(hr, L"UPLOAD buffer Map failed.");
        }

        auto* dstBase = static_cast<uint8_t*>(pMapped);
        auto* srcBase = static_cast<const uint8_t*>(pInitData);
        const uint32_t srcUseful = width * bpp;

        for (uint32_t row = 0; row < height; ++row) {
            memcpy(
                dstBase + static_cast<size_t>(row) * alignedPitch,
                srcBase + static_cast<size_t>(row) * srcRowPitch,
                srcUseful);

            // Zero padding bytes.
            if (alignedPitch > srcUseful) {
                memset(
                    dstBase + static_cast<size_t>(row) * alignedPitch + srcUseful,
                    0, alignedPitch - srcUseful);
            }
        }

        D3D12_RANGE writtenRange{0, bufferSize};
        outUpload->Unmap(0, &writtenRange);
    }

    // ── Record + execute CopyTextureRegion ───────────────────────────────
    {
        if (const HRESULT reset = m_ctx.GetCommandAllocator()->Reset(); FAILED(reset)) return DX12Result::Fail(reset, L"Command allocator reset failed.");
        if (const HRESULT reset = m_ctx.GetCommandList()->Reset(m_ctx.GetCommandAllocator(), nullptr); FAILED(reset)) return DX12Result::Fail(reset, L"Command list reset failed.");

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = outUpload.Get();
        srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset             = 0;
        srcLoc.PlacedFootprint.Footprint.Format   = format;
        srcLoc.PlacedFootprint.Footprint.Width    = width;
        srcLoc.PlacedFootprint.Footprint.Height   = height;
        srcLoc.PlacedFootprint.Footprint.Depth    = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = alignedPitch;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource        = outTexture.Get();
        dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        m_ctx.GetCommandList()->CopyTextureRegion(
            &dstLoc, 0, 0, 0, &srcLoc, nullptr);

        // Transition to shader resource so NGX can read it.
        TransitionBarrier(
            m_ctx.GetCommandList(), outTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (const HRESULT close = m_ctx.GetCommandList()->Close(); FAILED(close)) return DX12Result::Fail(close, L"Command list close failed.");
        ID3D12CommandList* lists[] = { m_ctx.GetCommandList() };
        m_ctx.GetCommandQueue()->ExecuteCommandLists(1, lists);
        if (const auto wait = m_ctx.WaitForGPU(); !wait) return wait;
    }

    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// AllocateSyntheticResources
// ═════════════════════════════════════════════════════════════════════════════
//
//  Creates and initialises the three synthetic DLSS input textures:
//
//  1. Depth buffer (R32_FLOAT) — filled uniformly with 1.0f.
//     "A synthetic depth buffer … uniformly filled with a constant depth
//      value of 1.0, representing the far plane, as spatial depth does not
//      exist in a flat 2D layer." — architecture.md
//
//  2. Motion vectors (R16G16_FLOAT) — initially zeroed.
//     Updated per-frame in the warmup loop with inverse jitter values.
//     "Synthesized R16G16_FLOAT vectors that perfectly counteract applied
//      sub-pixel jitter matrices." — architecture.md
//
//  3. Exposure texture (1×1 R16_FLOAT) — filled with 1.0f.
//     "Typically set to 1.0 for SDR." — architecture.md
//
//  Also allocates the ping-pong output textures for multi-pass.
//
DX12Result PipelineSimulator::AllocateSyntheticResources(
    uint32_t width, uint32_t height)
{
    if (m_synth.valid) {
        ReleaseSyntheticResources();
    }

    m_synth.width  = width;
    m_synth.height = height;

    // ── 1. Depth buffer: R32_FLOAT, all 1.0f ─────────────────────────────
    {
        const uint32_t bpp = BytesPerPixelFor(DXGI_FORMAT_R32_FLOAT); // 4
        const uint32_t srcPitch = width * bpp;
        std::vector<float> depthData(
            static_cast<size_t>(width) * height, 1.0f);

        auto result = CreateAndFillTexture(
            width, height, DXGI_FORMAT_R32_FLOAT,
            depthData.data(), srcPitch,
            m_synth.depthBuffer, m_synth.depthUpload);
        if (!result) {
            return DX12Result::Fail(result.hr,
                L"Failed to create/fill depth buffer.");
        }
    }

    // ── 2. Motion vectors: R16G16_FLOAT, zeroed ──────────────────────────
    //  Zeroed initially.  Per-frame values are written via UpdateMotionVectors.
    {
        const uint32_t bpp = BytesPerPixelFor(DXGI_FORMAT_R16G16_FLOAT); // 4
        const uint32_t srcPitch = width * bpp;
        std::vector<uint8_t> mvData(
            static_cast<size_t>(srcPitch) * height, 0);

        auto result = CreateAndFillTexture(
            width, height, DXGI_FORMAT_R16G16_FLOAT,
            mvData.data(), srcPitch,
            m_synth.motionVectors, m_synth.motionUpload);
        if (!result) {
            return DX12Result::Fail(result.hr,
                L"Failed to create/fill motion vectors.");
        }
    }

    // ── 3. Exposure texture: 1×1 R16_FLOAT, value = 1.0f ────────────────
    {
        // Half-float for 1.0f = 0x3C00
        uint16_t exposureHalf = 0x3C00;
        const uint32_t srcPitch = sizeof(uint16_t); // 2 bytes for 1 pixel

        auto result = CreateAndFillTexture(
            1, 1, DXGI_FORMAT_R16_FLOAT,
            &exposureHalf, srcPitch,
            m_synth.exposureTexture, m_synth.exposureUpload);
        if (!result) {
            return DX12Result::Fail(result.hr,
                L"Failed to create/fill exposure texture.");
        }
    }

    // ── 4. Ping-pong output textures: R16G16B16A16_FLOAT ─────────────────
    //  These receive the DLSS output.  Created in COMMON so they can be
    //  transitioned to UAV or COPY_DEST as needed.
    {
        auto r1 = CreateDefaultTexture(
            width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_STATE_COMMON, m_synth.pingTexture);
        if (!r1) return DX12Result::Fail(r1.hr, L"Ping texture alloc failed.");

        auto r2 = CreateDefaultTexture(
            width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_STATE_COMMON, m_synth.pongTexture);
        if (!r2) return DX12Result::Fail(r2.hr, L"Pong texture alloc failed.");
    }

    // ── 5. Normals texture: R16G16B16A16_FLOAT, flat perpendicular (0, 0, 1, 0)
    {
        const uint32_t bpp = BytesPerPixelFor(DXGI_FORMAT_R16G16B16A16_FLOAT); // 8
        const uint32_t srcPitch = width * bpp;
        std::vector<uint16_t> normalData(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < normalData.size(); i += 4) {
            normalData[i + 0] = 0x0000; // X = 0.0f
            normalData[i + 1] = 0x0000; // Y = 0.0f
            normalData[i + 2] = 0x3C00; // Z = 1.0f (unit normal out of 2D canvas)
            normalData[i + 3] = 0x0000; // W = 0.0f
        }
        auto result = CreateAndFillTexture(
            width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
            normalData.data(), srcPitch,
            m_synth.normalsTexture, m_synth.normalsUpload);
        if (!result) return DX12Result::Fail(result.hr, L"Failed to create/fill synthetic normals.");
    }

    // ── 6. Roughness texture: R16G16B16A16_FLOAT, neutral 0.5f ──────────
    {
        const uint32_t bpp = BytesPerPixelFor(DXGI_FORMAT_R16G16B16A16_FLOAT); // 8
        const uint32_t srcPitch = width * bpp;
        std::vector<uint16_t> roughnessData(static_cast<size_t>(width) * height * 4, 0x3800); // 0.5f in FP16
        auto result = CreateAndFillTexture(
            width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
            roughnessData.data(), srcPitch,
            m_synth.roughnessTexture, m_synth.roughnessUpload);
        if (!result) return DX12Result::Fail(result.hr, L"Failed to create/fill synthetic roughness.");
    }

    // ── 7. Specular albedo texture: R16G16B16A16_FLOAT, dielectric 0.04f ─
    {
        const uint32_t bpp = BytesPerPixelFor(DXGI_FORMAT_R16G16B16A16_FLOAT); // 8
        const uint32_t srcPitch = width * bpp;
        std::vector<uint16_t> specData(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < specData.size(); i += 4) {
            specData[i + 0] = 0x291F; // ~0.04f in FP16
            specData[i + 1] = 0x291F;
            specData[i + 2] = 0x291F;
            specData[i + 3] = 0x3C00; // 1.0f
        }
        auto result = CreateAndFillTexture(
            width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
            specData.data(), srcPitch,
            m_synth.specularAlbedoTexture, m_synth.specularUpload);
        if (!result) return DX12Result::Fail(result.hr, L"Failed to create/fill synthetic specular albedo.");
    }

    m_synth.valid = true;
    OutputDebugStringW(
        L"[Gaslightr] PipelineSimulator: Synthetic resources (including DLSS-D GBuffer) allocated.\n");
    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// UpdateMotionVectors
// ═════════════════════════════════════════════════════════════════════════════
//
//  Writes uniform inverse-jitter motion vectors to the motion vector texture.
//
//  "Instead of calculating true optical expansion (which would yield zero
//   for a static image), the plugin writes uniform motion vectors across
//   the entire buffer that perfectly counteract the applied sub-pixel
//   jitter." — architecture.md
//
//  The motion vector values are (-jitterX, -jitterY) in pixel space,
//  written as R16G16_FLOAT (two half-floats per pixel).
//
DX12Result PipelineSimulator::UpdateMotionVectors(
    float jitterX, float jitterY)
{
    // Convert the inverse jitter to half-float.
    // The jitter values are already in pixel space (before normalisation).
    // Motion vectors = exact inverse = (-jitterX, -jitterY).
    float mvX = -jitterX;
    float mvY = -jitterY;

    // Simple float-to-half conversion (same logic as MemoryBridge).
    auto floatToHalf = [](float value) -> uint16_t {
        uint32_t f;
        memcpy(&f, &value, sizeof(f));
        uint32_t sign     = (f >> 16) & 0x8000;
        int32_t  exponent = static_cast<int32_t>((f >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = f & 0x007FFFFF;
        if (exponent <= 0) {
            if (exponent < -10) return static_cast<uint16_t>(sign);
            mantissa = (mantissa | 0x00800000) >> (1 - exponent);
            return static_cast<uint16_t>(sign | (mantissa >> 13));
        }
        if (exponent == 0xFF - (127 - 15)) {
            return static_cast<uint16_t>(
                sign | 0x7C00 | (mantissa == 0 ? 0 : (mantissa >> 13)));
        }
        if (exponent > 30) return static_cast<uint16_t>(sign | 0x7C00);
        return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
    };

    uint16_t halfMVX = floatToHalf(mvX);
    uint16_t halfMVY = floatToHalf(mvY);

    // ── Fill the upload buffer with uniform (mvX, mvY) values ────────────
    const uint32_t bpp = BytesPerPixelFor(DXGI_FORMAT_R16G16_FLOAT); // 4
    const uint32_t alignedPitch =
        MemoryBridge::CalculateAlignedPitch(m_synth.width, bpp);
    const uint64_t bufferSize =
        static_cast<uint64_t>(alignedPitch) * m_synth.height;

    void* pMapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    HRESULT hr = m_synth.motionUpload->Map(0, &readRange, &pMapped);
    if (FAILED(hr)) {
        return DX12Result::Fail(hr, L"Motion vector upload Map failed.");
    }

    auto* dstBase = static_cast<uint8_t*>(pMapped);
    for (uint32_t row = 0; row < m_synth.height; ++row) {
        auto* dst = reinterpret_cast<uint16_t*>(
            dstBase + static_cast<size_t>(row) * alignedPitch);
        for (uint32_t x = 0; x < m_synth.width; ++x) {
            dst[x * 2 + 0] = halfMVX;
            dst[x * 2 + 1] = halfMVY;
        }
        // Zero padding.
        uint32_t useful = m_synth.width * bpp;
        if (alignedPitch > useful) {
            memset(
                dstBase + static_cast<size_t>(row) * alignedPitch + useful,
                0, alignedPitch - useful);
        }
    }

    D3D12_RANGE writtenRange{0, bufferSize};
    m_synth.motionUpload->Unmap(0, &writtenRange);

    // ── Copy upload → motion vector texture ──────────────────────────────
    if (const HRESULT reset = m_ctx.GetCommandAllocator()->Reset(); FAILED(reset)) return DX12Result::Fail(reset, L"Command allocator reset failed.");
    if (const HRESULT reset = m_ctx.GetCommandList()->Reset(m_ctx.GetCommandAllocator(), nullptr); FAILED(reset)) return DX12Result::Fail(reset, L"Command list reset failed.");

    // Transition MV texture to COPY_DEST.
    TransitionBarrier(
        m_ctx.GetCommandList(), m_synth.motionVectors.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = m_synth.motionUpload.Get();
    srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset             = 0;
    srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R16G16_FLOAT;
    srcLoc.PlacedFootprint.Footprint.Width    = m_synth.width;
    srcLoc.PlacedFootprint.Footprint.Height   = m_synth.height;
    srcLoc.PlacedFootprint.Footprint.Depth    = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = alignedPitch;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource        = m_synth.motionVectors.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    m_ctx.GetCommandList()->CopyTextureRegion(
        &dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition back to shader resource.
    TransitionBarrier(
        m_ctx.GetCommandList(), m_synth.motionVectors.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (const HRESULT close = m_ctx.GetCommandList()->Close(); FAILED(close)) return DX12Result::Fail(close, L"Command list close failed.");
    ID3D12CommandList* lists[] = { m_ctx.GetCommandList() };
    m_ctx.GetCommandQueue()->ExecuteCommandLists(1, lists);
    if (const auto wait = m_ctx.WaitForGPU(); !wait) return wait;

    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// ExecuteFullPipeline
// ═════════════════════════════════════════════════════════════════════════════
//
//  The core temporal spoofing pipeline:
//
//  STAGE 1 — TEMPORAL WARMUP
//    For each frame 0..warmupFrames-1:
//      1. Compute Halton jitter:
//           jitterX = Halton(frame+1, 2) - 0.5f
//           jitterY = Halton(frame+1, 3) - 0.5f
//      2. If useInverseMotion, update motion vectors to (-jitterX, -jitterY).
//      3. Fill NVNGX_EvaluationInputs with:
//           - pColorInput    = the input tile
//           - pMotionVectors = inverse-jitter MV texture
//           - pDepthBuffer   = flat depth at 1.0f
//           - CameraJitterX  = jitterX / width   (normalised)
//           - CameraJitterY  = jitterY / height   (normalised)
//           - FrameIndex     = frame
//           - ResetHistory   = (frame == 0)
//      4. EvaluateNR → ping texture (warmup output)
//      5. WaitForGPU before next frame.
//
//  STAGE 2 — MULTI-PASS ENHANCEMENT
//    For each pass 0..enhancePasses-1:
//      1. Input  = (pass==0) ? warmup output (ping) : previous output
//      2. Output = opposite buffer (pong or ping)
//      3. New jitter from continued Halton sequence
//      4. EvaluateNR → output buffer
//      5. WaitForGPU
//    ppOutputTexture receives the final result buffer.
//
DX12Result PipelineSimulator::EvaluateFrame(const NVNGX_EvaluationInputs& inputs,
    ID3D12Resource* output, bool validationOnly)
{
    if (!validationOnly) {
        auto parameters = inputs;
        return NeuralBackend::GetInstance().Evaluate(m_ctx.GetCommandList(), &parameters, output);
    }
    if (!inputs.pColorInput || !output || inputs.pColorInput == output)
        return DX12Result::Fail(E_INVALIDARG, L"Validation copy needs distinct input and output textures.");
    const auto source = inputs.pColorInput->GetDesc();
    const auto destination = output->GetDesc();
    if (source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        destination.Dimension != source.Dimension || source.Format != GPU_TEXTURE_FORMAT ||
        destination.Format != source.Format || destination.Width != source.Width ||
        destination.Height != source.Height || source.MipLevels != 1 || destination.MipLevels != 1 ||
        source.DepthOrArraySize != 1 || destination.DepthOrArraySize != 1 ||
        source.SampleDesc.Count != 1 || destination.SampleDesc.Count != 1)
        return DX12Result::Fail(E_INVALIDARG, L"Validation copy texture descriptors do not match.");
    const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto* list = m_ctx.GetCommandList();
    TransitionBarrier(list, inputs.pColorInput, readable, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionBarrier(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(output, inputs.pColorInput);
    TransitionBarrier(list, inputs.pColorInput, D3D12_RESOURCE_STATE_COPY_SOURCE, readable);
    TransitionBarrier(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The enclosing frame closes/submits the list and checks its fence.
    return DX12Result::Success();
}

DX12Result PipelineSimulator::InitializeComputePipeline()
{
    if (m_computePSO && m_computeRootSig && m_computeDescHeap) {
        return DX12Result::Success();
    }

    ID3D12Device* device = m_ctx.GetDevice();
    if (!device) {
        return DX12Result::Fail(E_POINTER, L"Null D3D12 device.");
    }

    // Parameter 0: 32-bit constants (b0) -> width, height, intensity, structure
    D3D12_ROOT_PARAMETER rootParams[3]{};

    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 4;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 1: Descriptor Table for Input Texture SRV (t0)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Parameter 2: Descriptor Table for Output Texture UAV (u0)
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
    rootSigDesc.NumParameters = 3;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 0;
    rootSigDesc.pStaticSamplers = nullptr;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serializedRootSig, &errorBlob);
    if (FAILED(hr)) {
        return DX12Result::Fail(hr, L"Failed to serialize compute root signature.");
    }

    hr = device->CreateRootSignature(0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&m_computeRootSig));
    if (FAILED(hr)) {
        return DX12Result::Fail(hr, L"Failed to create compute root signature.");
    }

    // Compute Pipeline State Object
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = m_computeRootSig.Get();
    psoDesc.CS.pShaderBytecode = g_EnhanceCS_Bytecode;
    psoDesc.CS.BytecodeLength = sizeof(g_EnhanceCS_Bytecode);
    psoDesc.NodeMask = 0;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_computePSO));
    if (FAILED(hr)) {
        return DX12Result::Fail(hr, L"Failed to create compute pipeline state.");
    }

    // Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 16;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_computeDescHeap));
    if (FAILED(hr)) {
        return DX12Result::Fail(hr, L"Failed to create compute descriptor heap.");
    }

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return DX12Result::Success();
}

DX12Result PipelineSimulator::DispatchEnhancePass(
    ID3D12Resource* pInputTexture,
    ID3D12Resource* pOutputTexture,
    uint32_t width,
    uint32_t height,
    float intensity,
    float structure,
    D3D12_RESOURCE_STATES outputStateBefore)
{
    if (!pInputTexture || !pOutputTexture) {
        return DX12Result::Fail(E_INVALIDARG, L"DispatchEnhancePass: null texture arguments.");
    }

    auto initRes = InitializeComputePipeline();
    if (!initRes) return initRes;

    ID3D12Device* device = m_ctx.GetDevice();
    const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Slot 0: SRV for input
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandleSRV = m_computeDescHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    device->CreateShaderResourceView(pInputTexture, &srvDesc, cpuHandleSRV);

    // Slot 1: UAV for output
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandleUAV = cpuHandleSRV;
    cpuHandleUAV.ptr += m_descriptorSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    device->CreateUnorderedAccessView(pOutputTexture, nullptr, &uavDesc, cpuHandleUAV);

    if (const HRESULT reset = m_ctx.GetCommandAllocator()->Reset(); FAILED(reset))
        return DX12Result::Fail(reset, L"Command allocator reset failed.");
    if (const HRESULT reset = m_ctx.GetCommandList()->Reset(m_ctx.GetCommandAllocator(), nullptr); FAILED(reset))
        return DX12Result::Fail(reset, L"Command list reset failed.");

    auto* list = m_ctx.GetCommandList();

    // Transition output to UAV
    TransitionBarrier(list, pOutputTexture, outputStateBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    list->SetPipelineState(m_computePSO.Get());
    list->SetComputeRootSignature(m_computeRootSig.Get());

    ID3D12DescriptorHeap* heaps[] = { m_computeDescHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);

    struct {
        uint32_t width;
        uint32_t height;
        float intensity;
        float structure;
    } filterConstants{ width, height, intensity, structure };
    list->SetComputeRoot32BitConstants(0, 4, &filterConstants, 0);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandleSRV = m_computeDescHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandleUAV = gpuHandleSRV;
    gpuHandleUAV.ptr += m_descriptorSize;

    list->SetComputeRootDescriptorTable(1, gpuHandleSRV);
    list->SetComputeRootDescriptorTable(2, gpuHandleUAV);

    const uint32_t groupsX = (width + 15) / 16;
    const uint32_t groupsY = (height + 15) / 16;
    list->Dispatch(groupsX, groupsY, 1);

    // Transition output to readable for subsequent readback or shader sampling
    TransitionBarrier(list, pOutputTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable);

    if (const HRESULT close = list->Close(); FAILED(close))
        return DX12Result::Fail(close, L"Command list close failed.");
    ID3D12CommandList* lists[] = { list };
    m_ctx.GetCommandQueue()->ExecuteCommandLists(1, lists);
    if (const auto wait = m_ctx.WaitForGPU(); !wait) return wait;

    return DX12Result::Success();
}

DX12Result PipelineSimulator::ExecuteFullPipeline(
    ID3D12Resource*         pInputTexture,
    ID3D12Resource**        ppOutputTexture,
    const SimulatorConfig&  config)
{
    if (!m_synth.valid) {
        return DX12Result::Fail(E_NOT_VALID_STATE,
            L"Synthetic resources not allocated.  "
            L"Call AllocateSyntheticResources first.");
    }
    if (!pInputTexture || !ppOutputTexture) {
        return DX12Result::Fail(E_INVALIDARG,
            L"ExecuteFullPipeline — null argument.");
    }

    *ppOutputTexture = nullptr;
    if (m_consumed) {
        return DX12Result::Fail(E_NOT_VALID_STATE, L"Allocate fresh synthetic resources before processing another tile.");
    }
    m_consumed = true;

    const uint32_t w = m_synth.width;
    const uint32_t h = m_synth.height;
    const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // ── Diagnostic / Validation Copy Fallback ────────────────────────────
    if (config.validationOnly) {
        if (const HRESULT reset = m_ctx.GetCommandAllocator()->Reset(); FAILED(reset)) return DX12Result::Fail(reset, L"Command allocator reset failed.");
        if (const HRESULT reset = m_ctx.GetCommandList()->Reset(m_ctx.GetCommandAllocator(), nullptr); FAILED(reset)) return DX12Result::Fail(reset, L"Command list reset failed.");

        NVNGX_EvaluationInputs inputs{};
        inputs.pColorInput = pInputTexture;

        TransitionBarrier(m_ctx.GetCommandList(), m_synth.pingTexture.Get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto evalResult = EvaluateFrame(inputs, m_synth.pingTexture.Get(), true);
        if (!evalResult) return evalResult;

        TransitionBarrier(m_ctx.GetCommandList(), m_synth.pingTexture.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable);

        if (const HRESULT close = m_ctx.GetCommandList()->Close(); FAILED(close)) return DX12Result::Fail(close, L"Command list close failed.");
        ID3D12CommandList* lists[] = { m_ctx.GetCommandList() };
        m_ctx.GetCommandQueue()->ExecuteCommandLists(1, lists);
        if (const auto wait = m_ctx.WaitForGPU(); !wait) return wait;

        *ppOutputTexture = m_synth.pingTexture.Get();
        return DX12Result::Success();
    }

    // ── Direct3D 12 Compute Shader Local Enhancement (Phase 8A/8B) ───────
    if (!kNeuralInferenceAvailable) {
        ID3D12Resource* currentInput = pInputTexture;
        ID3D12Resource* currentOutput = m_synth.pingTexture.Get();
        const uint32_t passes = (config.enhancePasses > 0) ? config.enhancePasses : 1;

        for (uint32_t pass = 0; pass < passes; ++pass) {
            currentOutput = (pass % 2 == 0) ? m_synth.pingTexture.Get() : m_synth.pongTexture.Get();
            D3D12_RESOURCE_STATES beforeState = (pass < 2) ? D3D12_RESOURCE_STATE_COMMON : readable;
            auto enhanceResult = DispatchEnhancePass(
                currentInput, currentOutput,
                w, h, config.intensity, config.structure, beforeState);
            if (!enhanceResult) return enhanceResult;
            currentInput = currentOutput;
        }

        *ppOutputTexture = currentOutput;
        return DX12Result::Success();
    }

    if (config.warmupFrames < 24 || config.warmupFrames > 32 ||
        config.enhancePasses < 1 || config.enhancePasses > 4) {
        return DX12Result::Fail(E_INVALIDARG, L"Expected 24-32 warmup frames and 1-4 enhancement passes.");
    }

    NeuralBackend& backend = NeuralBackend::GetInstance();
    if (!backend.IsReady()) {
        return DX12Result::Fail(E_NOT_VALID_STATE,
            L"NeuralBackend is not ready.");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // STAGE 1:  TEMPORAL WARMUP
    // ═══════════════════════════════════════════════════════════════════════
    //
    //  Reference: photoshop-dlssnr-blueprint.md §Component C
    //    "Warmup iterations: Feeds the static source texture continuously
    //     to resolve history weights"
    //
    OutputDebugStringW(
        L"[Gaslightr] Starting temporal warmup...\n");

    for (uint32_t frame = 0; frame < config.warmupFrames; ++frame) {

        // ── Halton jitter (base 2 for X, base 3 for Y) ──────────────────
        //  "jitterX = GetHaltonValue(frame + 1, 2) - 0.5f"
        //  "jitterY = GetHaltonValue(frame + 1, 3) - 0.5f"
        float jitterX = GetHaltonValue(static_cast<int>(frame) + 1, 2) - 0.5f;
        float jitterY = GetHaltonValue(static_cast<int>(frame) + 1, 3) - 0.5f;

        // ── Update motion vectors with exact inverse of jitter ───────────
        //  "Write uniform values to the Motion Vector texture that are
        //   the exact mathematical inverse of the jitter."
        if (config.useInverseMotion) {
            auto mvResult = UpdateMotionVectors(jitterX, jitterY);
            if (!mvResult) return mvResult;
        }

        // ── Record the evaluate command ──────────────────────────────────
        if (const HRESULT reset = m_ctx.GetCommandAllocator()->Reset(); FAILED(reset)) return DX12Result::Fail(reset, L"Command allocator reset failed.");
        if (const HRESULT reset = m_ctx.GetCommandList()->Reset(m_ctx.GetCommandAllocator(), nullptr); FAILED(reset)) return DX12Result::Fail(reset, L"Command list reset failed.");

        NVNGX_EvaluationInputs inputs{};
        inputs.pColorInput      = pInputTexture;
        inputs.pMotionVectors   = m_synth.motionVectors.Get();
        inputs.pDepthBuffer     = m_synth.depthBuffer.Get();
        inputs.pNormals         = m_synth.normalsTexture.Get();
        inputs.pRoughness       = m_synth.roughnessTexture.Get();
        inputs.pDiffuseAlbedo   = pInputTexture;
        inputs.pSpecularAlbedo  = m_synth.specularAlbedoTexture.Get();
        inputs.MotionVectorScaleX = 1.0f;
        inputs.MotionVectorScaleY = 1.0f;
        // Normalise jitter by resolution (blueprint convention).
        inputs.CameraJitterX    = jitterX / static_cast<float>(w);
        inputs.CameraJitterY    = jitterY / static_cast<float>(h);
        inputs.FrameIndex       = frame;
        inputs.ResetHistory     = (frame == 0);  // "Force clear on frame 0"

        TransitionBarrier(m_ctx.GetCommandList(), m_synth.pingTexture.Get(),
            frame == 0 ? D3D12_RESOURCE_STATE_COMMON : readable,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto evalResult = EvaluateFrame(inputs, m_synth.pingTexture.Get(), config.validationOnly);

        if (!evalResult) {
            wchar_t msg[256];
            swprintf_s(msg,
                L"Pipeline evaluation failed on warmup frame %u.", frame);
            return DX12Result::Fail(evalResult.hr, msg);
        }

        TransitionBarrier(m_ctx.GetCommandList(), m_synth.pingTexture.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable);
        // ── Execute + wait (mandatory per-frame sync) ────────────────────
        //  "Block execution thread until GPU pass completes before
        //   starting the next frame." — blueprint §Component C
        if (const HRESULT close = m_ctx.GetCommandList()->Close(); FAILED(close)) return DX12Result::Fail(close, L"Command list close failed.");
        ID3D12CommandList* lists[] = { m_ctx.GetCommandList() };
        m_ctx.GetCommandQueue()->ExecuteCommandLists(1, lists);
        if (const auto wait = m_ctx.WaitForGPU(); !wait) return wait;
    }

    OutputDebugStringW(
        L"[Gaslightr] Temporal warmup complete.\n");

    // ═══════════════════════════════════════════════════════════════════════
    // STAGE 2:  MULTI-PASS ENHANCEMENT  (ping-pong)
    // ═══════════════════════════════════════════════════════════════════════
    //
    //  Reference: architecture.md §Multi-Pass Rendering Loops
    //    "The original image tile is evaluated through DLSS with the first
    //     jitter matrix.  The output is fed back into the evaluate function
    //     as the new 'previous frame' history, accompanied by the second
    //     jitter matrix."
    //
    //  Ping-pong scheme:
    //    Pass 0: input = ping (warmup output) → output = pong
    //    Pass 1: input = pong                 → output = ping
    //    Pass 2: input = ping                 → output = pong
    //    ...
    //
    ID3D12Resource* currentInput  = m_synth.pingTexture.Get();
    ID3D12Resource* currentOutput = m_synth.pongTexture.Get();

    if (config.enhancePasses > 0) {
        OutputDebugStringW(
            L"[Gaslightr] Starting multi-pass enhancement...\n");
    }

    for (uint32_t pass = 0; pass < config.enhancePasses; ++pass) {

        // Continue the Halton sequence beyond the warmup frames.
        int haltonIndex = static_cast<int>(config.warmupFrames + pass) + 1;
        float jitterX = GetHaltonValue(haltonIndex, 2) - 0.5f;
        float jitterY = GetHaltonValue(haltonIndex, 3) - 0.5f;

        if (config.useInverseMotion) {
            auto mvResult = UpdateMotionVectors(jitterX, jitterY);
            if (!mvResult) return mvResult;
        }

        if (const HRESULT reset = m_ctx.GetCommandAllocator()->Reset(); FAILED(reset)) return DX12Result::Fail(reset, L"Command allocator reset failed.");
        if (const HRESULT reset = m_ctx.GetCommandList()->Reset(m_ctx.GetCommandAllocator(), nullptr); FAILED(reset)) return DX12Result::Fail(reset, L"Command list reset failed.");

        NVNGX_EvaluationInputs inputs{};
        inputs.pColorInput      = currentInput;
        inputs.pMotionVectors   = m_synth.motionVectors.Get();
        inputs.pDepthBuffer     = m_synth.depthBuffer.Get();
        inputs.pNormals         = m_synth.normalsTexture.Get();
        inputs.pRoughness       = m_synth.roughnessTexture.Get();
        inputs.pDiffuseAlbedo   = currentInput;
        inputs.pSpecularAlbedo  = m_synth.specularAlbedoTexture.Get();
        inputs.MotionVectorScaleX = 1.0f;
        inputs.MotionVectorScaleY = 1.0f;
        inputs.CameraJitterX    = jitterX / static_cast<float>(w);
        inputs.CameraJitterY    = jitterY / static_cast<float>(h);
        inputs.FrameIndex       = config.warmupFrames + pass;
        inputs.ResetHistory     = false;  // Never reset during enhancement.

        TransitionBarrier(m_ctx.GetCommandList(), currentOutput,
            pass == 0 ? D3D12_RESOURCE_STATE_COMMON : readable,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto evalResult = EvaluateFrame(inputs, currentOutput, config.validationOnly);

        if (!evalResult) {
            wchar_t msg[256];
            swprintf_s(msg,
                L"Pipeline evaluation failed on enhancement pass %u.", pass);
            return DX12Result::Fail(evalResult.hr, msg);
        }

        TransitionBarrier(m_ctx.GetCommandList(), currentOutput,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable);
        if (const HRESULT close = m_ctx.GetCommandList()->Close(); FAILED(close)) return DX12Result::Fail(close, L"Command list close failed.");
        ID3D12CommandList* lists[] = { m_ctx.GetCommandList() };
        m_ctx.GetCommandQueue()->ExecuteCommandLists(1, lists);
        if (const auto wait = m_ctx.WaitForGPU(); !wait) return wait;

        // ── Swap ping ↔ pong for the next pass ──────────────────────────
        ID3D12Resource* temp = currentInput;
        currentInput  = currentOutput;
        currentOutput = temp;
    }

    // ── Return the final output ──────────────────────────────────────────
    //  After the loops, currentInput holds the most recent output.
    //  (If enhancePasses == 0, currentInput is still ping = warmup output.)
    *ppOutputTexture = currentInput;

    OutputDebugStringW(
        L"[Gaslightr] Full temporal pipeline complete.\n");
    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// ReleaseSyntheticResources
// ═════════════════════════════════════════════════════════════════════════════
void PipelineSimulator::ReleaseSyntheticResources()
{
    m_consumed = false;
    // Must flush before releasing any GPU resources.
    if (m_synth.valid) {
        m_ctx.WaitForGPU();
    }

    m_synth.depthBuffer.Reset();
    m_synth.motionVectors.Reset();
    m_synth.exposureTexture.Reset();
    m_synth.normalsTexture.Reset();
    m_synth.roughnessTexture.Reset();
    m_synth.specularAlbedoTexture.Reset();
    m_synth.pingTexture.Reset();
    m_synth.pongTexture.Reset();
    m_synth.depthUpload.Reset();
    m_synth.motionUpload.Reset();
    m_synth.exposureUpload.Reset();
    m_synth.normalsUpload.Reset();
    m_synth.roughnessUpload.Reset();
    m_synth.specularUpload.Reset();

    m_synth.width  = 0;
    m_synth.height = 0;
    m_synth.valid  = false;
}

} // namespace Gaslightr
