#pragma once
// =============================================================================
// Gaslightr — MemoryBridge.h
// Zero-copy DX12 texture upload with 256-byte row pitch alignment.
//
// References:
//   • Photoshop DLSS 5 Plugin Architecture.md §Zero-Copy Memory Management
//   • dlss5_photoshop_blueprint.md §Phase 2 (256-Byte Pitch Alignment)
//   • photoshop-dlssnr-blueprint.md §4 (Memory Staging Interleave)
//
// This module bridges CPU-side raw pixel tiles (e.g. 1024×1024 from
// Photoshop's PSImageTileSuite) into GPU-resident VRAM textures formatted
// as DXGI_FORMAT_R16G16B16A16_FLOAT for the DLSS neural rendering pipeline.
//
// Architecture:
//   1. Caller provides a raw pixel buffer + dimensions + source format.
//   2. MemoryBridge allocates an intermediate UPLOAD heap with 256-byte
//      aligned row pitch, maps the CPU data into it, then issues
//      CopyTextureRegion to a DEFAULT heap (VRAM) texture.
//   3. A matching READBACK heap + ReadbackFromGPU method retrieves
//      processed results back to CPU memory after the NGX pass.
// =============================================================================

#include <windows.h>
#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <vector>

#include "DX12Context.h"

namespace Gaslightr {

// ── Source pixel format of incoming CPU data ─────────────────────────────────
//  Photoshop layers are typically 8-bit or 16-bit integer channels.
//  We normalise everything to R16G16B16A16_FLOAT on the GPU side.
enum class SourcePixelFormat : uint8_t {
    RGBA_8BIT,      // 4 bytes/pixel  — standard 8-bit SDR
    RGBA_16BIT,     // 8 bytes/pixel  — 16-bit integer (Photoshop 16-bit mode)
    RGBA_32F,       // 16 bytes/pixel — already floating-point (32-bit mode)
};

// ── Per-tile GPU resource bundle ─────────────────────────────────────────────
//  Holds the matched Upload → Default → Readback resource trio for one tile.
struct TileResources {
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;    // UPLOAD heap (staging)
    Microsoft::WRL::ComPtr<ID3D12Resource> gpuTexture;      // DEFAULT heap (VRAM)
    Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;  // READBACK heap

    uint32_t width        = 0;
    uint32_t height       = 0;
    uint32_t alignedPitch = 0;      // 256-byte aligned row pitch

    // Total byte size of the upload/readback buffers (alignedPitch * height).
    uint64_t bufferSize   = 0;

    bool valid = false;
};

// ── Memory alignment constants ──────────────────────────────────────────────
//
//  D3D12 requires texture upload row pitches to be aligned to a 256-byte
//  boundary.  The architecture.md defines the formula as:
//
//    AlignedPitch = (Width * BytesPerPixel + 255) & ~255
//
//  Failure to respect this causes "distorted texture reads, sheared image
//  data, or driver crashes during the upload phase" (ref 15).
//
static constexpr uint32_t DX12_TEXTURE_DATA_PITCH_ALIGNMENT = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT; // 256

// ── The final GPU texture format for all DLSS inputs ─────────────────────────
//  "DLSS requires HDR linear float precision" — blueprint §Phase 2.
//  All incoming tiles are normalised to this format regardless of source.
static constexpr DXGI_FORMAT GPU_TEXTURE_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Bytes per pixel for the GPU-side format (4 channels × 2 bytes = 8).
static constexpr uint32_t GPU_BYTES_PER_PIXEL = 8;

class MemoryBridge {
public:
    explicit MemoryBridge(DX12Context& ctx);
    ~MemoryBridge();

    // Non-copyable.
    MemoryBridge(const MemoryBridge&)            = delete;
    MemoryBridge& operator=(const MemoryBridge&) = delete;

    // ── Allocate tile resources ──────────────────────────────────────────
    //
    //  Creates the Upload, Default, and Readback resources for a tile of
    //  the given dimensions.  The upload/readback buffers are sized to the
    //  256-byte aligned pitch × height.  The Default texture is created as
    //  R16G16B16A16_FLOAT.
    //
    //  Returns a TileResources bundle (valid == true on success).
    //
    TileResources AllocateTile(uint32_t width, uint32_t height);

    // ── Upload CPU pixels → GPU VRAM ─────────────────────────────────────
    //
    //  1. Maps the UPLOAD buffer.
    //  2. Copies the raw pixel data row-by-row, applying 256-byte pitch
    //     padding and normalising the source format to half-float.
    //  3. Unmaps the buffer.
    //  4. Records a CopyTextureRegion command on the provided command list
    //     to transfer from the UPLOAD buffer to the DEFAULT texture.
    //
    //  The caller must execute the command list and WaitForGPU() before
    //  the GPU texture is valid for downstream use.
    //
    DX12Result UploadToGPU(
        TileResources&              tile,
        const void*                 pSrcPixels,
        uint32_t                    srcRowPitch,
        SourcePixelFormat           srcFormat,
        ID3D12GraphicsCommandList*  pCmdList);

    // ── Readback GPU VRAM → CPU ──────────────────────────────────────────
    //
    //  Records a CopyTextureRegion from the DEFAULT texture into the
    //  READBACK buffer.  After the caller executes + WaitForGPU(), call
    //  MapReadback() to access the results.
    //
    DX12Result ReadbackFromGPU(
        TileResources&              tile,
        ID3D12GraphicsCommandList*  pCmdList);

    // ── Map the readback buffer for CPU access ───────────────────────────
    //
    //  Returns a pointer to the mapped readback data (R16G16B16A16_FLOAT,
    //  256-byte aligned rows).  outRowPitch receives the aligned pitch.
    //  The caller must call UnmapReadback() when done.
    //
    DX12Result MapReadback(
        TileResources&  tile,
        void**          ppData,
        uint32_t*       outRowPitch);

    void UnmapReadback(TileResources& tile);

    // ── Utility: compute aligned pitch ───────────────────────────────────
    //
    //  AlignedPitch = (width * bytesPerPixel + 255) & ~255
    //
    //  This is the exact bitwise formula from the architecture document.
    //
    static constexpr uint32_t CalculateAlignedPitch(
        uint32_t width, uint32_t bytesPerPixel) noexcept
    {
        return (width * bytesPerPixel + (DX12_TEXTURE_DATA_PITCH_ALIGNMENT - 1))
             & ~(DX12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    }

private:
    DX12Context& m_ctx;

    // ── Internal helpers ─────────────────────────────────────────────────
    DX12Result CreateUploadBuffer(uint64_t sizeBytes,
        Microsoft::WRL::ComPtr<ID3D12Resource>& outResource);

    DX12Result CreateDefaultTexture(uint32_t width, uint32_t height,
        DXGI_FORMAT format,
        Microsoft::WRL::ComPtr<ID3D12Resource>& outResource);

    DX12Result CreateReadbackBuffer(uint64_t sizeBytes,
        Microsoft::WRL::ComPtr<ID3D12Resource>& outResource);
};

} // namespace Gaslightr
