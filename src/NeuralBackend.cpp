#include "NeuralBackend.h"

namespace Gaslightr {
NeuralBackend& NeuralBackend::GetInstance() {
    static NeuralBackend instance;
    return instance;
}

bool NeuralBackend::Initialize(const wchar_t*, ID3D12Device*, NVNGX_InitializationParams*) {
    Shutdown();
    m_lastError = L"DLSS-NR ABI is unverified. Use the explicit D3D12 validation-copy pipeline.";
    // DLL presence cannot enable an unverified ABI. Even legacy callers fail
    // before LoadLibrary, DllMain, export resolution or any vendor code runs.
    return false;
}

DX12Result NeuralBackend::Evaluate(ID3D12GraphicsCommandList*,
    NVNGX_EvaluationInputs*, ID3D12Resource*) {
    return DX12Result::Fail(E_NOTIMPL,
        L"Neural inference is disabled: no verified DLSS-NR integration contract.");
}

void NeuralBackend::Shutdown() {
    m_ready.store(false, std::memory_order_release);
    m_loaded.store(false, std::memory_order_release);
    m_lastError.clear();
}

NeuralBackend::~NeuralBackend() { Shutdown(); }
}
