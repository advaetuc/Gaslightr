// =============================================================================
// Gaslightr — MemoryBridge.cpp
// Zero-copy DX12 texture upload with 256-byte row pitch alignment.
//
// References:
//   • Photoshop DLSS 5 Plugin Architecture.md §Zero-Copy Memory Management
//       "The row pitch must be aligned to a 256-byte boundary in DX12.
//        AlignedPitch = (Width * BytesPerPixel + 255) & ~255"
//   • dlss5_photoshop_blueprint.md §Phase 2 (256-Byte Pitch Alignment)
//       "Failure to implement this exact bitwise logic will cause GPU
//        memory shearing."
//   • photoshop-dlssnr-blueprint.md §4 (Memory Staging Interleave)
//       "You must implement intermediate Readback and Upload staging
//        buffers (D3D12_HEAP_TYPE_READBACK / D3D12_HEAP_TYPE_UPLOAD)."
//   • dlss5_photoshop_blueprint.md §Phase 2 (Format Normalization)
//       "Map 8-bit or 16-bit Photoshop pixel data into
//        DXGI_FORMAT_R16G16B16A16_FLOAT."
//
// Pipeline per tile:
//
//   CPU pixels  ──Map──▶  UPLOAD buffer  ──CopyTextureRegion──▶  DEFAULT texture
//                         (256-byte pitch)                       (R16G16B16A16_FLOAT)
//                                                                      │
//                                                  [NGX EvaluateNR operates here]
//                                                                      │
//   CPU readback ◀──Map──  READBACK buffer ◀──CopyTextureRegion──  DEFAULT texture
//
// =============================================================================

#include "MemoryBridge.h"

#include <cassert>
#include <cstring>   // memcpy
#include <cmath>     // for 8-bit → half conversion
#include <algorithm> // std::min

namespace Gaslightr {

// ─────────────────────────────────────────────────────────────────────────────
// IEEE 754 half-precision helpers
// ─────────────────────────────────────────────────────────────────────────────
//
//  We need to convert 8-bit [0..255] and 16-bit [0..65535] integer pixel
//  channels to half-float (16-bit float) for the R16G16B16A16_FLOAT GPU
//  texture.  MSVC intrinsics are unavailable without DirectXMath, so we
//  use a compact software float-to-half converter.
//

// Convert a 32-bit float to a 16-bit half-float (IEEE 754 binary16).
static uint16_t FloatToHalf(float value) noexcept
{
    // Reinterpret float bits.
    uint32_t f;
    static_assert(sizeof(float) == sizeof(uint32_t));
    memcpy(&f, &value, sizeof(f));

    uint32_t sign     = (f >> 16) & 0x8000;
    int32_t  exponent = static_cast<int32_t>((f >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = f & 0x007FFFFF;

    if (exponent <= 0) {
        if (exponent < -10) {
            // Too small → zero.
            return static_cast<uint16_t>(sign);
        }
        // Denormalised half.
        mantissa = (mantissa | 0x00800000) >> (1 - exponent);
        return static_cast<uint16_t>(sign | (mantissa >> 13));
    }
    if (exponent == 0xFF - (127 - 15)) {
        // Inf / NaN.
        if (mantissa == 0) {
            return static_cast<uint16_t>(sign | 0x7C00); // Inf
        }
        return static_cast<uint16_t>(sign | 0x7C00 | (mantissa >> 13)); // NaN
    }
    if (exponent > 30) {
        // Overflow → Inf.
        return static_cast<uint16_t>(sign | 0x7C00);
    }

    return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
}

// ── Pre-computed LUT: 8-bit integer [0..255] → half-float ────────────────────
//  Built once, used on every 8-bit tile upload.  Each entry is the half-float
//  representation of value/255.0f.
static uint16_t g_8bitToHalfLUT[256];
static bool     g_lutInitialised = false;

static void EnsureLUT() noexcept
{
    if (g_lutInitialised) return;
    for (int i = 0; i < 256; ++i) {
        g_8bitToHalfLUT[i] = FloatToHalf(static_cast<float>(i) / 255.0f);
    }
    g_lutInitialised = true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ═════════════════════════════════════════════════════════════════════════════
MemoryBridge::MemoryBridge(DX12Context& ctx)
    : m_ctx(ctx)
{
    EnsureLUT();
}

MemoryBridge::~MemoryBridge() = default;

// ═════════════════════════════════════════════════════════════════════════════
// CreateUploadBuffer
// ═════════════════════════════════════════════════════════════════════════════
//
//  Allocates a D3D12_HEAP_TYPE_UPLOAD committed resource (linear buffer).
//  This is the CPU-writable staging area for texture row data.
//
DX12Result MemoryBridge::CreateUploadBuffer(
    uint64_t sizeBytes,
    Microsoft::WRL::ComPtr<ID3D12Resource>& outResource)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type                 = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Alignment          = 0;
    bufDesc.Width              = sizeBytes;
    bufDesc.Height             = 1;
    bufDesc.DepthOrArraySize   = 1;
    bufDesc.MipLevels          = 1;
    bufDesc.Format             = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count   = 1;
    bufDesc.SampleDesc.Quality = 0;
    bufDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = m_ctx.GetDevice()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,  // Upload buffers start readable.
        nullptr,
        IID_PPV_ARGS(&outResource));

    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"Failed to create UPLOAD buffer.");
    }
    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// CreateDefaultTexture
// ═════════════════════════════════════════════════════════════════════════════
//
//  Allocates a D3D12_HEAP_TYPE_DEFAULT committed texture (VRAM-resident).
//  This is the GPU-only resource that DLSS reads from and writes to.
//
//  Format is always R16G16B16A16_FLOAT for HDR linear float precision.
//
DX12Result MemoryBridge::CreateDefaultTexture(
    uint32_t width, uint32_t height, DXGI_FORMAT format,
    Microsoft::WRL::ComPtr<ID3D12Resource>& outResource)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type                 = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment          = 0;
    texDesc.Width              = width;
    texDesc.Height             = height;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = format;
    texDesc.SampleDesc.Count   = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = m_ctx.GetDevice()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,  // Initial state for receiving upload.
        nullptr,
        IID_PPV_ARGS(&outResource));

    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"Failed to create DEFAULT texture.");
    }
    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// CreateReadbackBuffer
// ═════════════════════════════════════════════════════════════════════════════
//
//  Allocates a D3D12_HEAP_TYPE_READBACK committed resource.
//  Used to copy processed GPU texture data back to CPU-accessible memory.
//
DX12Result MemoryBridge::CreateReadbackBuffer(
    uint64_t sizeBytes,
    Microsoft::WRL::ComPtr<ID3D12Resource>& outResource)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type                 = D3D12_HEAP_TYPE_READBACK;
    heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Alignment          = 0;
    bufDesc.Width              = sizeBytes;
    bufDesc.Height             = 1;
    bufDesc.DepthOrArraySize   = 1;
    bufDesc.MipLevels          = 1;
    bufDesc.Format             = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count   = 1;
    bufDesc.SampleDesc.Quality = 0;
    bufDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = m_ctx.GetDevice()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&outResource));

    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"Failed to create READBACK buffer.");
    }
    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// AllocateTile
// ═════════════════════════════════════════════════════════════════════════════
//
//  Creates the complete Upload → Default → Readback resource trio for a
//  tile of the given width × height.
//
//  CRITICAL MATH — 256-byte pitch alignment:
//
//    AlignedPitch = (Width * BytesPerPixel + 255) & ~255
//
//  For R16G16B16A16_FLOAT (8 bytes/pixel) at 1024 px wide:
//    Raw pitch    = 1024 * 8 = 8192 bytes
//    AlignedPitch = (8192 + 255) & ~255 = 8192  (already aligned)
//
//  For 1000 px wide:
//    Raw pitch    = 1000 * 8 = 8000 bytes
//    AlignedPitch = (8000 + 255) & ~255 = 8192  (192 bytes padding)
//
TileResources MemoryBridge::AllocateTile(uint32_t width, uint32_t height)
{
    TileResources tile;
    tile.width  = width;
    tile.height = height;

    // ── Compute 256-byte aligned pitch ───────────────────────────────────
    //  Exact formula from architecture.md:
    //    AlignedPitch = (Width * BytesPerPixel + 255) & ~255
    tile.alignedPitch = CalculateAlignedPitch(width, GPU_BYTES_PER_PIXEL);

    // Total buffer size = aligned pitch × number of rows.
    tile.bufferSize = static_cast<uint64_t>(tile.alignedPitch) * height;

    // ── Allocate the three resources ─────────────────────────────────────
    auto result = CreateUploadBuffer(tile.bufferSize, tile.uploadBuffer);
    if (!result) {
        OutputDebugStringW(
            L"[Gaslightr] MemoryBridge::AllocateTile — UPLOAD alloc failed.\n");
        return tile;  // tile.valid remains false
    }

    result = CreateDefaultTexture(width, height, GPU_TEXTURE_FORMAT, tile.gpuTexture);
    if (!result) {
        OutputDebugStringW(
            L"[Gaslightr] MemoryBridge::AllocateTile — DEFAULT alloc failed.\n");
        return tile;
    }

    result = CreateReadbackBuffer(tile.bufferSize, tile.readbackBuffer);
    if (!result) {
        OutputDebugStringW(
            L"[Gaslightr] MemoryBridge::AllocateTile — READBACK alloc failed.\n");
        return tile;
    }

    tile.valid = true;
    return tile;
}

// ═════════════════════════════════════════════════════════════════════════════
// UploadToGPU
// ═════════════════════════════════════════════════════════════════════════════
//
//  Maps the UPLOAD buffer, copies CPU pixel data row-by-row with:
//    • 256-byte pitch padding (gap bytes zeroed),
//    • Format normalisation from the source format to R16G16B16A16_FLOAT.
//  Then records a CopyTextureRegion from the UPLOAD buffer to the DEFAULT
//  texture on the given command list.
//
//  The caller is responsible for:
//    1. Resetting the command allocator + command list before this call.
//    2. Closing + executing the command list after this call.
//    3. Calling DX12Context::WaitForGPU() before reading the GPU texture.
//
DX12Result MemoryBridge::UploadToGPU(
    TileResources&              tile,
    const void*                 pSrcPixels,
    uint32_t                    srcRowPitch,
    SourcePixelFormat           srcFormat,
    ID3D12GraphicsCommandList*  pCmdList)
{
    if (!tile.valid) {
        return DX12Result::Fail(E_INVALIDARG,
            L"MemoryBridge::UploadToGPU — tile is not valid.");
    }
    if (!pSrcPixels || !pCmdList) {
        return DX12Result::Fail(E_INVALIDARG,
            L"MemoryBridge::UploadToGPU — null argument.");
    }

    // ── Map the UPLOAD buffer ────────────────────────────────────────────
    void* pMapped = nullptr;
    D3D12_RANGE readRange{0, 0};  // We won't read from this resource on CPU.
    HRESULT hr = tile.uploadBuffer->Map(0, &readRange, &pMapped);
    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"Failed to map UPLOAD buffer.");
    }

    auto* dstBase = static_cast<uint8_t*>(pMapped);
    auto* srcBase = static_cast<const uint8_t*>(pSrcPixels);

    // ── Row-by-row copy with format conversion + pitch alignment ─────────
    //
    //  For each row:
    //    1. Read `width` pixels from the source in the source format.
    //    2. Convert each pixel to 4 × half-float (R16G16B16A16).
    //    3. Write to the destination at the aligned pitch offset.
    //    4. Any trailing padding bytes (up to the 256-byte boundary) are
    //       left as-is (the GPU ignores them per the D3D12 spec).
    //
    const uint32_t dstPixelSize = GPU_BYTES_PER_PIXEL; // 8 bytes
    const uint32_t dstRowUseful = tile.width * dstPixelSize;

    for (uint32_t row = 0; row < tile.height; ++row) {
        const uint8_t* srcRow = srcBase + static_cast<size_t>(row) * srcRowPitch;
        uint8_t*       dstRow = dstBase + static_cast<size_t>(row) * tile.alignedPitch;

        switch (srcFormat) {
        // ── 8-bit RGBA → R16G16B16A16_FLOAT ─────────────────────────────
        //  Each 4-byte pixel becomes 8 bytes (4 × uint16 half-float).
        //  Uses the pre-computed LUT for speed.
        case SourcePixelFormat::RGBA_8BIT: {
            auto* dst16 = reinterpret_cast<uint16_t*>(dstRow);
            for (uint32_t x = 0; x < tile.width; ++x) {
                const uint8_t r = srcRow[x * 4 + 0];
                const uint8_t g = srcRow[x * 4 + 1];
                const uint8_t b = srcRow[x * 4 + 2];
                const uint8_t a = srcRow[x * 4 + 3];

                dst16[x * 4 + 0] = g_8bitToHalfLUT[r];
                dst16[x * 4 + 1] = g_8bitToHalfLUT[g];
                dst16[x * 4 + 2] = g_8bitToHalfLUT[b];
                dst16[x * 4 + 3] = g_8bitToHalfLUT[a];
            }
            break;
        }
        // ── 16-bit RGBA → R16G16B16A16_FLOAT ────────────────────────────
        //  Each 8-byte pixel (4 × uint16) is normalised to [0..1] half-float.
        case SourcePixelFormat::RGBA_16BIT: {
            auto* src16 = reinterpret_cast<const uint16_t*>(srcRow);
            auto* dst16 = reinterpret_cast<uint16_t*>(dstRow);
            for (uint32_t x = 0; x < tile.width; ++x) {
                for (uint32_t c = 0; c < 4; ++c) {
                    float normalised =
                        static_cast<float>(src16[x * 4 + c]) / 65535.0f;
                    dst16[x * 4 + c] = FloatToHalf(normalised);
                }
            }
            break;
        }
        // ── 32-bit float RGBA → R16G16B16A16_FLOAT ──────────────────────
        //  Each 16-byte pixel (4 × float32) is truncated to half-float.
        case SourcePixelFormat::RGBA_32F: {
            auto* srcF = reinterpret_cast<const float*>(srcRow);
            auto* dst16 = reinterpret_cast<uint16_t*>(dstRow);
            for (uint32_t x = 0; x < tile.width; ++x) {
                for (uint32_t c = 0; c < 4; ++c) {
                    dst16[x * 4 + c] = FloatToHalf(srcF[x * 4 + c]);
                }
            }
            break;
        }
        }

        // Zero trailing padding bytes (difference between useful width
        // and aligned pitch).  Not strictly required by the GPU, but
        // prevents uninitialised-memory reads in debug tools.
        if (tile.alignedPitch > dstRowUseful) {
            memset(dstRow + dstRowUseful, 0,
                   tile.alignedPitch - dstRowUseful);
        }
    }

    // ── Unmap the UPLOAD buffer ──────────────────────────────────────────
    D3D12_RANGE writtenRange{0, tile.bufferSize};
    tile.uploadBuffer->Unmap(0, &writtenRange);

    // ── Record CopyTextureRegion: UPLOAD buffer → DEFAULT texture ────────
    //
    //  Source: the UPLOAD buffer, interpreted as a placed footprint with
    //          the 256-byte aligned row pitch.
    //  Destination: the DEFAULT texture (subresource 0).
    //
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = tile.uploadBuffer.Get();
    srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset             = 0;
    srcLoc.PlacedFootprint.Footprint.Format   = GPU_TEXTURE_FORMAT;
    srcLoc.PlacedFootprint.Footprint.Width    = tile.width;
    srcLoc.PlacedFootprint.Footprint.Height   = tile.height;
    srcLoc.PlacedFootprint.Footprint.Depth    = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = tile.alignedPitch;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource        = tile.gpuTexture.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    pCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // ── Transition the DEFAULT texture from COPY_DEST → PIXEL_SHADER_RESOURCE
    //    so it's ready for NGX to read.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = tile.gpuTexture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                   | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    pCmdList->ResourceBarrier(1, &barrier);

    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// ReadbackFromGPU
// ═════════════════════════════════════════════════════════════════════════════
//
//  Records a resource barrier (SHADER_RESOURCE → COPY_SOURCE) and a
//  CopyTextureRegion from the DEFAULT texture into the READBACK buffer.
//
//  After the caller executes the command list and calls WaitForGPU(),
//  the readback buffer can be mapped via MapReadback().
//
DX12Result MemoryBridge::ReadbackFromGPU(
    TileResources&              tile,
    ID3D12GraphicsCommandList*  pCmdList)
{
    if (!tile.valid || !pCmdList) {
        return DX12Result::Fail(E_INVALIDARG,
            L"MemoryBridge::ReadbackFromGPU — invalid tile or null cmdlist.");
    }

    // ── Transition DEFAULT texture → COPY_SOURCE ─────────────────────────
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = tile.gpuTexture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                   | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;

    pCmdList->ResourceBarrier(1, &barrier);

    // ── CopyTextureRegion: DEFAULT texture → READBACK buffer ─────────────
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource        = tile.gpuTexture.Get();
    srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = tile.readbackBuffer.Get();
    dstLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset             = 0;
    dstLoc.PlacedFootprint.Footprint.Format   = GPU_TEXTURE_FORMAT;
    dstLoc.PlacedFootprint.Footprint.Width    = tile.width;
    dstLoc.PlacedFootprint.Footprint.Height   = tile.height;
    dstLoc.PlacedFootprint.Footprint.Depth    = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = tile.alignedPitch;

    pCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    pCmdList->ResourceBarrier(1, &barrier);
    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// MapReadback / UnmapReadback
// ═════════════════════════════════════════════════════════════════════════════
//
//  After ReadbackFromGPU + Execute + WaitForGPU, the caller uses
//  MapReadback to get a CPU pointer to the result pixels.
//
//  The mapped data is R16G16B16A16_FLOAT with 256-byte aligned row pitch.
//  outRowPitch receives the aligned pitch so the caller can iterate rows.
//
DX12Result MemoryBridge::MapReadback(
    TileResources& tile,
    void**         ppData,
    uint32_t*      outRowPitch)
{
    if (!tile.valid || !ppData || !outRowPitch) {
        return DX12Result::Fail(E_INVALIDARG,
            L"MemoryBridge::MapReadback — invalid args.");
    }

    D3D12_RANGE readRange{0, tile.bufferSize};
    HRESULT hr = tile.readbackBuffer->Map(0, &readRange, ppData);
    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"Failed to map READBACK buffer.");
    }

    *outRowPitch = tile.alignedPitch;
    return DX12Result::Success();
}

void MemoryBridge::UnmapReadback(TileResources& tile)
{
    if (tile.readbackBuffer) {
        D3D12_RANGE emptyRange{0, 0};
        tile.readbackBuffer->Unmap(0, &emptyRange);
    }
}

} // namespace Gaslightr
