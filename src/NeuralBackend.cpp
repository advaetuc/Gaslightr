// =============================================================================
// Gaslightr — NeuralBackend.cpp
// Dynamic DLL loader for nvngx_dlssnr.dll — Phase 2 Implementation
//
// References:
//   • photoshop-dlssnr-blueprint.md §Component A (NeuralBackend.h/.cpp)
//   • dlss5_photoshop_blueprint.md  §Phase 4 (Dynamic Proxy Loading)
//   • Photoshop DLSS 5 Plugin Architecture.md §Required Binaries
//
// This module:
//   1. Loads nvngx_dlssnr.dll (or a proxy DLL) via LoadLibraryW.
//   2. Resolves PFN_NVNGX_INITIALIZE_NR and PFN_NVNGX_EVALUATE_NR via
//      GetProcAddress using the known exported symbol names.
//   3. Calls InitializeNR to configure the DLSS feature context.
//   4. Wraps EvaluateNR in a safe DX12Result-returning wrapper.
//
// FALLBACK SAFETY NET (blueprint §4):
//   "Code the wrapper pointers to return gracefully instead of crashing
//    the Photoshop host process if the internal binary fails to resolve,
//    exposes mismatched internal offsets, or throws an unhandled
//    structural exception."
//
//   We implement this via:
//     • nullptr checks on every function pointer before invocation.
//     • Win32 __try/__except around the actual DLL calls to catch
//       access violations from mismatched struct layouts.
//     • Atomic state flags so concurrent queries never see partial state.
// =============================================================================

#include "NeuralBackend.h"
#include <cstdio>

namespace Gaslightr {

// SEH must live in leaf functions without C++ objects requiring unwinding.
static int InvokeInitialize(PFN_NVNGX_INITIALIZE_NR fn, ID3D12Device* device,
    NVNGX_InitializationParams* params, DWORD* exception) noexcept {
    __try { return fn(device, params); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *exception = GetExceptionCode(); return 0; }
}
static int InvokeEvaluate(PFN_NVNGX_EVALUATE_NR fn, ID3D12GraphicsCommandList* list,
    NVNGX_EvaluationInputs* inputs, ID3D12Resource* output, DWORD* exception) noexcept {
    __try { return fn(list, inputs, output); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *exception = GetExceptionCode(); return 0; }
}

// ─────────────────────────────────────────────────────────────────────────────
// Expected export names from nvngx_dlssnr.dll
//
// These are the symbols that community wrappers (DLSS5-Feeder, OptiScaler,
// faisalkindi/DLSS5oneclick) expose.  The real nvngx_dlssnr.dll may or may
// not export them by these exact names; if not, the Initialize() call will
// gracefully fail and report the missing symbol.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr const char* kSymbolInitializeNR = "NVSDK_NGX_DLSSNR_InitializeNR";
static constexpr const char* kSymbolEvaluateNR   = "NVSDK_NGX_DLSSNR_EvaluateNR";

// ── Fallback symbol names (alternative export conventions) ───────────────────
//  Some wrappers use shortened names without the DLSSNR prefix.
static constexpr const char* kSymbolInitializeNR_Alt = "NVSDK_NGX_InitializeNR";
static constexpr const char* kSymbolEvaluateNR_Alt   = "NVSDK_NGX_EvaluateNR";

// ═════════════════════════════════════════════════════════════════════════════
// Singleton
// ═════════════════════════════════════════════════════════════════════════════
NeuralBackend& NeuralBackend::GetInstance()
{
    static NeuralBackend instance;
    return instance;
}

// ═════════════════════════════════════════════════════════════════════════════
// Initialize
// ═════════════════════════════════════════════════════════════════════════════
//
//  Step 1:  LoadLibraryW to load the neural rendering DLL.
//  Step 2:  GetProcAddress to resolve InitializeNR + EvaluateNR.
//  Step 3:  Call InitializeNR with the DX12 device and params.
//
//  On ANY failure, the function cleans up, stores a diagnostic string
//  in m_lastError, and returns false.  The host process is never crashed.
//
bool NeuralBackend::Initialize(
    const wchar_t*               dllPath,
    ID3D12Device*                pDevice,
    NVNGX_InitializationParams*  pInitParams)
{
    // ── Guard: already initialised ───────────────────────────────────────
    if (m_loaded.load(std::memory_order_acquire)) {
        m_lastError = L"NeuralBackend::Initialize called twice.";
        return false;
    }

    if (!dllPath || !pDevice || !pInitParams) {
        m_lastError = L"NeuralBackend::Initialize — null argument.";
        return false;
    }

    // ── Step 1: Load the DLL ─────────────────────────────────────────────
    //
    //  Reference: dlss5_photoshop_blueprint.md §Phase 4
    //    "Dynamically load nvngx.dll (or a proxy dxgi.dll)."
    //
    //  We use LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR so that any satellite
    //  dependencies (nvngx.dll, nvngx_dlss.dll) that sit next to the
    //  main binary are found without polluting the system PATH.
    //
    m_hModule = ::LoadLibraryExW(
        dllPath,
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    if (!m_hModule) {
        // Fallback: try plain LoadLibraryW (searches standard dirs).
        m_hModule = ::LoadLibraryW(dllPath);
    }

    if (!m_hModule) {
        DWORD err = ::GetLastError();
        wchar_t msg[512];
        swprintf_s(msg,
            L"LoadLibraryW(\"%s\") failed.  Win32 error: %lu.  "
            L"Ensure the DLL and its dependencies are present.",
            dllPath, err);
        m_lastError = msg;
        return false;
    }

    // ── Step 2: Resolve function pointers ────────────────────────────────
    //
    //  Reference: dlss5_photoshop_blueprint.md §Phase 4
    //    "Resolve function pointers PFN_NVNGX_INITIALIZE_NR and
    //     PFN_NVNGX_EVALUATE_NR manually via GetProcAddress.
    //     Ensure graceful fallback (return false; instead of crashing)
    //     if resolution fails."
    //
    //  We try the primary symbol names first, then the alternate names.
    //
    m_pfnInitNR = reinterpret_cast<PFN_NVNGX_INITIALIZE_NR>(
        ::GetProcAddress(m_hModule, kSymbolInitializeNR));
    if (!m_pfnInitNR) {
        m_pfnInitNR = reinterpret_cast<PFN_NVNGX_INITIALIZE_NR>(
            ::GetProcAddress(m_hModule, kSymbolInitializeNR_Alt));
    }

    m_pfnEvalNR = reinterpret_cast<PFN_NVNGX_EVALUATE_NR>(
        ::GetProcAddress(m_hModule, kSymbolEvaluateNR));
    if (!m_pfnEvalNR) {
        m_pfnEvalNR = reinterpret_cast<PFN_NVNGX_EVALUATE_NR>(
            ::GetProcAddress(m_hModule, kSymbolEvaluateNR_Alt));
    }

    if (!m_pfnInitNR) {
        wchar_t msg[512];
        swprintf_s(msg,
            L"GetProcAddress failed for InitializeNR.  "
            L"Tried symbols: \"%hs\" and \"%hs\".  "
            L"The DLL may be incompatible or corrupted.",
            kSymbolInitializeNR, kSymbolInitializeNR_Alt);
        m_lastError = msg;
        ::FreeLibrary(m_hModule);
        m_hModule = nullptr;
        return false;
    }

    if (!m_pfnEvalNR) {
        wchar_t msg[512];
        swprintf_s(msg,
            L"GetProcAddress failed for EvaluateNR.  "
            L"Tried symbols: \"%hs\" and \"%hs\".  "
            L"The DLL may be incompatible or corrupted.",
            kSymbolEvaluateNR, kSymbolEvaluateNR_Alt);
        m_lastError = msg;
        ::FreeLibrary(m_hModule);
        m_hModule   = nullptr;
        m_pfnInitNR = nullptr;
        return false;
    }

    m_loaded.store(true, std::memory_order_release);

    OutputDebugStringW(
        L"[Gaslightr] NeuralBackend: DLL loaded, function pointers resolved.\n");

    // ── Step 3: Call InitializeNR ────────────────────────────────────────
    //
    //  SAFETY NET:  We wrap the call in a structured exception handler
    //  (__try/__except) to catch access violations that would occur if
    //  the DLL's internal struct layout doesn't match our definition.
    //  This is the "Fallback Safety Net" from the blueprint.
    //
    DWORD exCode = 0;
    const int initResult = InvokeInitialize(m_pfnInitNR, pDevice, pInitParams, &exCode);
    if (exCode != 0) {
        wchar_t msg[512];
        swprintf_s(msg,
            L"InitializeNR threw a structured exception (code 0x%08lX).  "
            L"The DLL's internal struct layout likely mismatches our "
            L"NVNGX_InitializationParams definition.",
            exCode);
        Shutdown();
        m_lastError = msg;
        return false;
    }

    if (initResult != NVNGX_RESULT_SUCCESS) {
        wchar_t msg[256];
        swprintf_s(msg,
            L"InitializeNR returned error code %d.  "
            L"The NGX feature context failed to initialise.",
            initResult);
        m_lastError = msg;
        // Don't unload the DLL here — the caller may want to retry
        // with different params.  But mark as not ready.
        return false;
    }

    m_ready.store(true, std::memory_order_release);

    OutputDebugStringW(
        L"[Gaslightr] NeuralBackend: InitializeNR succeeded.  "
        L"Neural rendering context is READY.\n");

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Evaluate  (safe wrapper)
// ═════════════════════════════════════════════════════════════════════════════
//
//  Wraps PFN_NVNGX_EVALUATE_NR with null-pointer and SEH guards.
//  Returns DX12Result so callers can chain error handling without
//  catching C++ exceptions.
//
DX12Result NeuralBackend::Evaluate(
    ID3D12GraphicsCommandList* pCmdList,
    NVNGX_EvaluationInputs*    pInputs,
    ID3D12Resource*            pOutputSurface)
{
    if (!m_ready.load(std::memory_order_acquire)) {
        return DX12Result::Fail(E_NOT_VALID_STATE,
            L"NeuralBackend::Evaluate — backend is not ready.  "
            L"Call Initialize() first.");
    }
    if (!pCmdList || !pInputs || !pOutputSurface) {
        return DX12Result::Fail(E_INVALIDARG,
            L"NeuralBackend::Evaluate — null argument.");
    }
    if (!m_pfnEvalNR) {
        return DX12Result::Fail(E_POINTER,
            L"NeuralBackend::Evaluate — EvaluateNR pointer is null.");
    }

    DWORD exCode = 0;
    const int evalResult = InvokeEvaluate(m_pfnEvalNR, pCmdList, pInputs, pOutputSurface, &exCode);
    if (exCode != 0) {
        m_ready.store(false, std::memory_order_release);
        wchar_t msg[512];
        swprintf_s(msg,
            L"EvaluateNR threw a structured exception (code 0x%08lX).  "
            L"Possible struct mismatch or GPU resource state error.",
            exCode);
        m_lastError = msg;
        return DX12Result::Fail(
            HRESULT_FROM_WIN32(exCode), m_lastError);
    }

    if (evalResult != NVNGX_RESULT_SUCCESS) {
        wchar_t msg[256];
        swprintf_s(msg,
            L"EvaluateNR returned error code %d.", evalResult);
        m_lastError = msg;
        return DX12Result::Fail(E_FAIL, m_lastError);
    }

    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// Shutdown
// ═════════════════════════════════════════════════════════════════════════════
//
//  Safe to call multiple times.  Nulls all pointers and frees the DLL.
//
void NeuralBackend::Shutdown()
{
    m_ready.store(false, std::memory_order_release);
    m_loaded.store(false, std::memory_order_release);

    m_pfnInitNR = nullptr;
    m_pfnEvalNR = nullptr;

    if (m_hModule) {
        ::FreeLibrary(m_hModule);
        m_hModule = nullptr;
        OutputDebugStringW(
            L"[Gaslightr] NeuralBackend: DLL unloaded.\n");
    }

    m_lastError.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// Destructor
// ═════════════════════════════════════════════════════════════════════════════
NeuralBackend::~NeuralBackend()
{
    Shutdown();
}

} // namespace Gaslightr
