#pragma once
// =============================================================================
// Gaslightr — PipelineSimulator.h
// Temporal spoofing engine: synthesises fake 3D game-loop data to gaslight the
// DLSS 5 neural network into processing static 2D image tiles.
//
// References:
//   • photoshop-dlssnr-blueprint.md §Component C (Temporal Simulation)
//   • dlss5_photoshop_blueprint.md  §Phase 5 (Temporal Spoofing)
//   • Photoshop DLSS 5 Plugin Architecture.md §Adapting the Neural Pipeline
//     and §Multi-Pass Rendering Loops
//
// This module:
//   1. Generates synthetic DLSS input buffers:
//        - Depth buffer:    R32_FLOAT filled uniformly with 1.0f (far plane)
//        - Motion vectors:  R16G16_FLOAT, exact inverse of per-frame jitter
//        - Exposure:        1×1 R16_FLOAT at 1.0f (SDR default)
//   2. Implements the Halton sequence (base 2, base 3) for sub-pixel jitter.
//   3. Runs a 24–32 frame temporal warmup loop, calling NeuralBackend::Evaluate
//      per frame with GPU fence synchronisation between iterations.
//   4. Runs 1–4 multi-pass enhancement passes with ping-pong VRAM buffers,
//      feeding the output of pass N as the input to pass N+1.
// =============================================================================

#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>

#include "DX12Context.h"
#include "MemoryBridge.h"
#include "NeuralBackend.h"
#include "nvngx_dlssnr_core.h"

namespace Gaslightr {

// ── Simulator configuration ──────────────────────────────────────────────────
struct SimulatorConfig {
    // Explicit GPU copy validation: never invokes the neural runtime.
    bool validationOnly = false;
    // Explicit compute shader pipeline flag (Phase 11 fallback & local enhancement)
    bool useComputePipeline = false;
    // Number of temporal warmup frames.  The neural network needs 24–32
    // frames of jittered history before its temporal accumulation produces
    // high-quality output.  (blueprint: "exactly 24 to 32 times")
    uint32_t warmupFrames   = 24;

    // Number of multi-pass enhancement passes (1–4).
    // Each pass feeds its output back as the next pass's input.
    // (architecture.md: "1 to 4 passes, mirroring NODIX-TECH/DLSS-5-MANAGER")
    uint32_t enhancePasses  = 1;

    // If true, write inverse-jitter motion vectors each frame.
    // If false, motion vectors are zeroed (simpler but lower quality).
    bool     useInverseMotion = true;

    // Phase 8A/8B parameters for GPU compute enhancement:
    float    intensity = 0.0f;
    float    structure = 0.0f;
};

// ── Synthetic resource set ───────────────────────────────────────────────────
//  Pre-allocated GPU textures that remain resident across the entire
//  warmup + multi-pass loop.  Allocated once per tile resolution.
struct SyntheticResources {
    Microsoft::WRL::ComPtr<ID3D12Resource> depthBuffer;           // R32_FLOAT, 1.0f
    Microsoft::WRL::ComPtr<ID3D12Resource> motionVectors;         // R16G16_FLOAT
    Microsoft::WRL::ComPtr<ID3D12Resource> exposureTexture;       // 1×1 R16_FLOAT

    // Synthetic GBuffer textures for DLSS 3.5+ Ray Reconstruction (DLSS-D)
    Microsoft::WRL::ComPtr<ID3D12Resource> normalsTexture;       // R16G16B16A16_FLOAT, (0, 0, 1, 0)
    Microsoft::WRL::ComPtr<ID3D12Resource> roughnessTexture;     // R16G16B16A16_FLOAT, 0.5f neutral
    Microsoft::WRL::ComPtr<ID3D12Resource> specularAlbedoTexture;// R16G16B16A16_FLOAT, 0.04 dielectric

    // Ping-pong output textures for multi-pass.
    Microsoft::WRL::ComPtr<ID3D12Resource> pingTexture;           // R16G16B16A16_FLOAT
    Microsoft::WRL::ComPtr<ID3D12Resource> pongTexture;           // R16G16B16A16_FLOAT

    // Upload staging buffers for initial fill operations.
    Microsoft::WRL::ComPtr<ID3D12Resource> depthUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> motionUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> exposureUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> normalsUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> roughnessUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> specularUpload;

    uint32_t width  = 0;
    uint32_t height = 0;
    bool     valid  = false;
};

class PipelineSimulator {
public:
    explicit PipelineSimulator(DX12Context& ctx);
    ~PipelineSimulator();

    // Non-copyable.
    PipelineSimulator(const PipelineSimulator&)            = delete;
    PipelineSimulator& operator=(const PipelineSimulator&) = delete;

    // ── Halton sequence ──────────────────────────────────────────────────
    //
    //  Generates sub-pixel jitter values using the radical-inverse Halton
    //  sequence.  Base 2 for X, base 3 for Y.
    //
    //  Exact implementation from photoshop-dlssnr-blueprint.md §Component C.
    //
    static float GetHaltonValue(int index, int base) noexcept;

    // ── Allocate synthetic resources ─────────────────────────────────────
    //
    //  Creates the depth buffer, motion vector texture, exposure texture,
    //  and ping-pong output textures for the given tile dimensions.
    //
    //  The depth buffer is filled with 1.0f (infinite far plane).
    //  The exposure texture is filled with 1.0f (SDR neutral).
    //  Motion vectors are initially zeroed; they are updated per-frame
    //  during the warmup loop.
    //
    DX12Result AllocateSyntheticResources(uint32_t width, uint32_t height);

    // ── Execute the full temporal pipeline ───────────────────────────────
    //
    //  Runs:
    //    1. Temporal warmup (config.warmupFrames iterations)
    //    2. Multi-pass enhancement (config.enhancePasses iterations)
    //
    //  pInputTexture:   The uploaded tile in R16G16B16A16_FLOAT (from MemoryBridge).
    //  pOutputTexture:  Receives the final enhanced result.
    //                   May alias m_synth.pingTexture or pongTexture.
    //
    //  Returns DX12Result::Fail if the backend rejects any evaluation call.
    //
    DX12Result ExecuteFullPipeline(
        ID3D12Resource*     pInputTexture,
        ID3D12Resource**    ppOutputTexture,
        const SimulatorConfig& config);

    // ── Access the synthetic resources (for advanced callers) ─────────────
    const SyntheticResources& GetSyntheticResources() const noexcept {
        return m_synth;
    }

    // ── Release synthetic resources ──────────────────────────────────────
    void ReleaseSyntheticResources();

private:
    DX12Result EvaluateFrame(const NVNGX_EvaluationInputs& inputs,
        ID3D12Resource* output, bool validationOnly);
    DX12Context& m_ctx;
    SyntheticResources m_synth;
    bool m_consumed = false;

    // ── Compute Shader pipeline for local enhancement (Phase 8A/8B) ──────
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_computeRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_computePSO;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_computeDescHeap;
    uint32_t                                     m_descriptorSize = 0;

    DX12Result InitializeComputePipeline();
    DX12Result DispatchEnhancePass(
        ID3D12Resource* pInputTexture,
        ID3D12Resource* pOutputTexture,
        uint32_t width,
        uint32_t height,
        float intensity,
        float structure,
        D3D12_RESOURCE_STATES outputStateBefore = D3D12_RESOURCE_STATE_COMMON);

    // ── Internal: fill a GPU texture via upload buffer ────────────────────
    DX12Result CreateAndFillTexture(
        uint32_t width, uint32_t height,
        DXGI_FORMAT format,
        const void* pInitData,
        uint32_t    srcRowPitch,
        Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture,
        Microsoft::WRL::ComPtr<ID3D12Resource>& outUpload);

    DX12Result CreateDefaultTexture(
        uint32_t width, uint32_t height,
        DXGI_FORMAT format,
        D3D12_RESOURCE_STATES initialState,
        Microsoft::WRL::ComPtr<ID3D12Resource>& outResource);

    // ── Internal: update motion vectors for a given jitter ────────────────
    DX12Result UpdateMotionVectors(float jitterX, float jitterY);

    // ── Internal: transition a resource between states ────────────────────
    static void TransitionBarrier(
        ID3D12GraphicsCommandList* pCmdList,
        ID3D12Resource* pResource,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after);
};

} // namespace Gaslightr
