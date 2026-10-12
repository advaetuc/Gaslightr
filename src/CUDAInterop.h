#pragma once
// =============================================================================
// Gaslightr — CUDAInterop.h
// CUDA ↔ DX12 shared-memory interop and CIELAB Detail-Only Mode.
//
// References:
//   • Photoshop DLSS 5 Plugin Architecture.md §Direct3D 12 and CUDA Interop
//       — IDXGIResource1::CreateSharedHandle → cudaImportExternalMemory
//       — cudaExternalMemoryGetMappedMipmappedArray
//   • Photoshop DLSS 5 Plugin Architecture.md §CIELAB Color Space
//       — Linear RGB → XYZ → CIELAB (L*, a*, b*)
//       — Detail-Only: extract L* → DLSS → recombine with original a*, b*
//   • dlss5_photoshop_blueprint.md §Phase 6 (Detail-Only Mode)
//
// Pipeline:
//
//   DX12 R16G16B16A16_FLOAT tile
//       │
//       ├── CreateSharedHandle ──► cudaImportExternalMemory
//       │                              │
//       │                     CUDA kernel: RGB→XYZ→CIELAB
//       │                              │
//       │                    ┌─────────┴─────────┐
//       │                    │                     │
//       │              L* channel              a*, b* channels
//       │              (grayscale)             (stored in VRAM)
//       │                    │
//       │              [DLSS Pass]
//       │                    │
//       │              Enhanced L*
//       │                    │
//       │                    └─────────┬─────────┘
//       │                              │
//       │                     CUDA kernel: CIELAB→XYZ→RGB
//       │                              │
//       └────────────────── result back to DX12
//
// =============================================================================

#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>

#include "DX12Context.h"

namespace Gaslightr {

// ── CIELAB conversion result textures ────────────────────────────────────────
struct CIELABResources {
    // L* channel extracted as a single-channel float texture.
    // This is what gets fed to the DLSS pipeline in Detail-Only Mode.
    Microsoft::WRL::ComPtr<ID3D12Resource> luminanceTexture;   // R16_FLOAT

    // a* and b* chrominance channels stored in VRAM during the DLSS pass.
    // These are NOT processed by DLSS — they are held unchanged.
    Microsoft::WRL::ComPtr<ID3D12Resource> chromaTexture;      // R16G16_FLOAT (a*, b*)

    // Recombined output after DLSS-enhanced L* + original a*, b*.
    Microsoft::WRL::ComPtr<ID3D12Resource> recombinedTexture;  // R16G16B16A16_FLOAT

    uint32_t width  = 0;
    uint32_t height = 0;
    bool     valid  = false;
};

class CUDAInterop {
public:
    explicit CUDAInterop(DX12Context& ctx);
    ~CUDAInterop();

    // Non-copyable.
    CUDAInterop(const CUDAInterop&)            = delete;
    CUDAInterop& operator=(const CUDAInterop&) = delete;

    // ── Initialise CUDA context ──────────────────────────────────────────
    //  Finds the CUDA device matching the DX12 adapter and creates a
    //  CUDA context on it.  Returns false if CUDA is unavailable.
    bool InitializeCUDA();

    // ── Allocate CIELAB resources ────────────────────────────────────────
    //  Creates the L*, a*b*, and recombined textures for a tile.
    DX12Result AllocateCIELABResources(uint32_t width, uint32_t height);

    // ── Forward pass: RGB → CIELAB, extract L* ───────────────────────────
    //
    //  1. Exports the DX12 color texture via CreateSharedHandle.
    //  2. Imports into CUDA via cudaImportExternalMemory.
    //  3. Runs the RGB→XYZ→CIELAB CUDA kernel.
    //  4. Writes L* to luminanceTexture, a*b* to chromaTexture.
    //
    //  After this call, luminanceTexture is ready to be used as the
    //  DLSS input (as a grayscale R16_FLOAT tile).
    //
    DX12Result DecomposeToLuminance(ID3D12Resource* pColorTexture);

    // ── Inverse pass: recombine enhanced L* + original a*b* → RGB ────────
    //
    //  1. Reads the DLSS-enhanced L* from the provided texture.
    //  2. Reads the stored a*b* from chromaTexture.
    //  3. Runs the CIELAB→XYZ→RGB CUDA kernel.
    //  4. Writes the result to recombinedTexture.
    //
    DX12Result RecombineFromLuminance(ID3D12Resource* pEnhancedLuminance);

    // ── Access results ───────────────────────────────────────────────────
    const CIELABResources& GetResources() const noexcept { return m_cielab; }
    ID3D12Resource* GetLuminanceTexture() const noexcept {
        return m_cielab.luminanceTexture.Get();
    }
    ID3D12Resource* GetRecombinedTexture() const noexcept {
        return m_cielab.recombinedTexture.Get();
    }

    // ── Cleanup ──────────────────────────────────────────────────────────
    void ReleaseCIELABResources();
    void ShutdownCUDA();

    bool IsCUDAAvailable() const noexcept { return m_cudaInitialised; }

private:
    DX12Context& m_ctx;
    CIELABResources m_cielab;
    bool m_cudaInitialised = false;
    int  m_cudaDevice      = -1;
};

// ═════════════════════════════════════════════════════════════════════════════
// CIELAB Mathematics (host-side reference — CUDA kernels use device versions)
// ═════════════════════════════════════════════════════════════════════════════
//
//  All formulas from Photoshop DLSS 5 Plugin Architecture.md §CIELAB:
//
//  ── Linear RGB → XYZ (sRGB/D65 matrix) ─────────────────────────────────
//
//    | X |   | 0.4124564  0.3575761  0.1804375 |   | R |
//    | Y | = | 0.2126729  0.7151522  0.0721750 | × | G |
//    | Z |   | 0.0193339  0.1191920  0.9503041 |   | B |
//
//  ── XYZ → CIELAB ───────────────────────────────────────────────────────
//
//    L* = 116 · f(Y / Yn) - 16
//    a* = 500 · (f(X / Xn) - f(Y / Yn))
//    b* = 200 · (f(Y / Yn) - f(Z / Zn))
//
//    where:
//      f(t) = t^(1/3)                          if t > (6/29)³ ≈ 0.008856
//      f(t) = (1/3)(29/6)² · t + 4/29          otherwise
//
//    D65 reference white:
//      Xn = 0.950456,  Yn = 1.0,  Zn = 1.088754
//
//  ── CIELAB → XYZ (inverse) ─────────────────────────────────────────────
//
//    Y = Yn · f⁻¹((L* + 16) / 116)
//    X = Xn · f⁻¹((L* + 16) / 116 + a* / 500)
//    Z = Zn · f⁻¹((L* + 16) / 116 - b* / 200)
//
//    where:
//      f⁻¹(t) = t³                             if t > 6/29
//      f⁻¹(t) = 3(6/29)² · (t - 4/29)         otherwise
//
//  ── XYZ → Linear RGB (inverse sRGB matrix) ─────────────────────────────
//
//    | R |   |  3.2404542  -1.5371385  -0.4985314 |   | X |
//    | G | = | -0.9692660   1.8760108   0.0415560 | × | Y |
//    | B |   |  0.0556434  -0.2040259   1.0572252 |   | Z |
//
// ═════════════════════════════════════════════════════════════════════════════

} // namespace Gaslightr
