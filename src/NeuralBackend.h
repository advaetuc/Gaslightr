#pragma once
// =============================================================================
// Gaslightr — NeuralBackend.h
// Production implementation of NVIDIA NGX DLSS and DLSS-D Ray Reconstruction
// Direct3D 12 bridge.
//
// SAFETY INVARIANT: Inside Adobe Photoshop host runtime, neural inference
// remains gated OFF by default (kNeuralInferenceAvailable = false in
// BackendCapabilities.h). When kNeuralInferenceAvailable is false, Initialize()
// returns false before loading any vendor binary, protecting host stability.
// =============================================================================

#include <atomic>
#include <memory>
#include <string>
#include <windows.h>
#include <d3d12.h>

#include "nvngx_dlssnr_core.h"
#include "nvngx_d3d12.h"
#include "DX12Context.h"

namespace Gaslightr {

class NeuralBackend {
public:
    static NeuralBackend& GetInstance();

    // Standard lifecycle called by TileProcessor / PipelineSimulator
    bool Initialize(const wchar_t* dllPath, ID3D12Device* device,
        NVNGX_InitializationParams* parameters);
    DX12Result Evaluate(ID3D12GraphicsCommandList* commandList,
        NVNGX_EvaluationInputs* inputs, ID3D12Resource* output);
    void Shutdown();

    // Explicit NGX activation for isolated test harnesses and validation
    DX12Result InitializeExplicit(ID3D12Device* device, uint32_t inWidth, uint32_t inHeight,
        uint32_t outWidth, uint32_t outHeight, bool enableRayReconstruction, int perfQuality = 2);
    DX12Result EvaluateExplicit(ID3D12GraphicsCommandList* commandList,
        const NVNGX_EvaluationInputs& inputs, ID3D12Resource* output);

    bool IsLoaded() const noexcept { return m_loaded.load(std::memory_order_acquire); }
    bool IsReady() const noexcept { return m_ready.load(std::memory_order_acquire); }
    const std::wstring& GetLastError() const noexcept { return m_lastError; }

private:
    NeuralBackend() = default;
    ~NeuralBackend();
    NeuralBackend(const NeuralBackend&) = delete;
    NeuralBackend& operator=(const NeuralBackend&) = delete;

    std::atomic<bool> m_loaded{false};
    std::atomic<bool> m_ready{false};
    std::wstring m_lastError;

    struct NgxSession;
    std::unique_ptr<NgxSession> m_session;
};

} // namespace Gaslightr
