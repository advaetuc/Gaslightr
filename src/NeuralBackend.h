#pragma once
#include <atomic>
#include <string>
#include "nvngx_dlssnr_core.h"
#include "DX12Context.h"

namespace Gaslightr {
// Disabled vendor boundary. PipelineSimulator owns the separate validation
// implementation; this class never reports copies as neural inference.
// TileProcessor serializes calls, including lifecycle and error access.
class NeuralBackend {
public:
    static NeuralBackend& GetInstance();
    bool Initialize(const wchar_t* dllPath, ID3D12Device* device,
        NVNGX_InitializationParams* parameters);
    DX12Result Evaluate(ID3D12GraphicsCommandList* commandList,
        NVNGX_EvaluationInputs* inputs, ID3D12Resource* output);
    void Shutdown();
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
};
}
