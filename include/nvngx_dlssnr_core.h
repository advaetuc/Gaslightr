#pragma once
// =============================================================================
// Gaslightr — nvngx_dlssnr_core.h
// Inferred / extracted NGX parameter structs and function pointer typedefs.
//
// Source:  photoshop-dlssnr-blueprint.md §Component A (NeuralBackend.h)
//
// These are the exact structural layouts derived from standard NVNGX wrappers
// and community reverse-engineering (DLSS5-Feeder, OptiScaler).  They will be
// populated in Phase 4 (NeuralBackend.cpp) when we dynamically resolve the
// entry points from nvngx_dlssnr.dll via GetProcAddress.
// =============================================================================

#include <windows.h>
#include <d3d12.h>
#include <cstdint>

namespace Gaslightr {

// ── NVNGX Initialization Parameters ─────────────────────────────────────────
//
//  Passed to PFN_NVNGX_INITIALIZE_NR to configure the DLSS feature context.
//  Width/Height are the source (input) resolution.
//  TargetWidth/TargetHeight are the desired output resolution.
//  PerformanceMode selects the neural network quality preset.
//
struct NVNGX_InitializationParams {
    unsigned int Width;
    unsigned int Height;
    unsigned int TargetWidth;
    unsigned int TargetHeight;
    int          PerformanceMode;   // 0 = Quality, 1 = Balanced, 2 = Performance
};

// ── NVNGX Evaluation Inputs ─────────────────────────────────────────────────
//
//  Per-frame inputs fed to PFN_NVNGX_EVALUATE_NR.
//  For static 2D images, the motion vectors are the exact inverse of the
//  applied jitter, and the depth buffer is flat at 1.0f (infinite far plane).
//
struct NVNGX_EvaluationInputs {
    ID3D12Resource* pColorInput;        // R16G16B16A16_FLOAT — the source tile
    ID3D12Resource* pMotionVectors;     // R16G16_FLOAT       — inverse jitter
    ID3D12Resource* pDepthBuffer;       // R32_FLOAT          — flat at 1.0f
    ID3D12Resource* pNormals;           // R16G16B16A16_FLOAT — synthetic normals (0, 0, 1)
    ID3D12Resource* pRoughness;         // R16G16B16A16_FLOAT — synthetic roughness
    ID3D12Resource* pDiffuseAlbedo;     // R16G16B16A16_FLOAT — diffuse albedo
    ID3D12Resource* pSpecularAlbedo;    // R16G16B16A16_FLOAT — specular albedo (0.04)
    float           MotionVectorScaleX; // Typically 1.0f
    float           MotionVectorScaleY; // Typically 1.0f
    float           CameraJitterX;      // Halton(frame, 2) - 0.5f, normalised
    float           CameraJitterY;      // Halton(frame, 3) - 0.5f, normalised
    unsigned int    FrameIndex;         // Monotonic frame counter
    bool            ResetHistory;       // true ONLY on frame 0
};

// ── Function pointer typedefs for dynamic resolution ─────────────────────────
//
//  Resolved from nvngx_dlssnr.dll (or its proxy) via GetProcAddress.
//  If resolution fails, the pointers remain nullptr and the backend
//  must return gracefully (no crash — ref: blueprint §Fallback Safety Net).
//
using PFN_NVNGX_INITIALIZE_NR = int(*)(
    ID3D12Device*              pDevice,
    NVNGX_InitializationParams* pParams);

using PFN_NVNGX_EVALUATE_NR = int(*)(
    ID3D12GraphicsCommandList* pCmdList,
    NVNGX_EvaluationInputs*    pInputs,
    ID3D12Resource*            pOutputSurface);

} // namespace Gaslightr
