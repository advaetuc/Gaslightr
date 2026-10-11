#pragma once
// =============================================================================
// Gaslightr — NeuralBackend.h
// Dynamic loader for nvngx_dlssnr.dll — resolves InitializeNR / EvaluateNR
// entry points via GetProcAddress at runtime.
//
// Source:  photoshop-dlssnr-blueprint.md §Component A
//          dlss5_photoshop_blueprint.md  §Phase 4 (DLSS Security Bypass)
//
// Design:
//   • Singleton — a single DLL handle per process lifetime.
//   • Zero-exception — all failure paths return bool/DX12Result, never throw.
//   • Host-safe — if the DLL fails to load or offsets mismatch, the backend
//     enters a "dead" state and every subsequent call is a safe no-op.
// =============================================================================

#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>

#include <cstdint>
#include <string>
#include <atomic>

#include "nvngx_dlssnr_core.h"
#include "DX12Context.h"

namespace Gaslightr {

// ── NR feature status codes returned by the DLL entry points ─────────────────
//  These mirror the NVSDK_NGX_Result convention: 1 = success, anything
//  else is a feature-specific failure code.
static constexpr int NVNGX_RESULT_SUCCESS = 1;

class NeuralBackend {
public:
    // ── Singleton accessor ───────────────────────────────────────────────
    //  Thread-safe via C++11 static initialisation guarantee (§6.7/4).
    static NeuralBackend& GetInstance();

    // ── Lifecycle ────────────────────────────────────────────────────────
    //
    //  Initialize():
    //    1. LoadLibraryW(dllPath) to load nvngx_dlssnr.dll.
    //    2. GetProcAddress to resolve PFN_NVNGX_INITIALIZE_NR and
    //       PFN_NVNGX_EVALUATE_NR by their exported symbol names.
    //    3. Calls InitializeNR with the DX12 device and the provided
    //       initialization parameters.
    //    Returns false (not throw) on any failure.
    //
    //  Shutdown():
    //    Calls FreeLibrary and nulls all function pointers.
    //    Safe to call multiple times.
    //
    bool Initialize(const wchar_t*          dllPath,
                    ID3D12Device*           pDevice,
                    NVNGX_InitializationParams* pInitParams);

    void Shutdown();

    // ── Runtime query ────────────────────────────────────────────────────
    bool IsLoaded()  const noexcept { return m_loaded.load(std::memory_order_acquire); }
    bool IsReady()   const noexcept { return m_ready.load(std::memory_order_acquire); }

    // ── Evaluation wrapper ───────────────────────────────────────────────
    //
    //  Safe wrapper around PFN_NVNGX_EVALUATE_NR.
    //  Returns DX12Result::Fail if the backend is not ready or the DLL
    //  call returns a non-success code.  Never throws.
    //
    DX12Result Evaluate(ID3D12GraphicsCommandList* pCmdList,
                        NVNGX_EvaluationInputs*    pInputs,
                        ID3D12Resource*            pOutputSurface);

    // ── Last-error diagnostic ────────────────────────────────────────────
    const std::wstring& GetLastError() const noexcept { return m_lastError; }

private:
    NeuralBackend() = default;
    ~NeuralBackend();

    // Non-copyable.
    NeuralBackend(const NeuralBackend&)            = delete;
    NeuralBackend& operator=(const NeuralBackend&) = delete;

    // ── DLL handle & resolved function pointers ─────────────────────────
    HMODULE                  m_hModule      = nullptr;
    PFN_NVNGX_INITIALIZE_NR m_pfnInitNR    = nullptr;
    PFN_NVNGX_EVALUATE_NR   m_pfnEvalNR    = nullptr;

    // ── State flags ─────────────────────────────────────────────────────
    std::atomic<bool> m_loaded{false};   // DLL loaded + pointers resolved
    std::atomic<bool> m_ready{false};    // InitializeNR succeeded

    // ── Diagnostic ──────────────────────────────────────────────────────
    std::wstring m_lastError;
};

} // namespace Gaslightr
