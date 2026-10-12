// =============================================================================
// Gaslightr — DX12Context.cpp
// Headless Direct3D 12 Environment — Phase 1 Implementation
//
// References:
//   • photoshop-dlssnr-blueprint.md  §Component B (Headless Execution)
//   • Photoshop DLSS 5 Plugin Architecture.md  §Phase 1 (Environment)
//   • dlss5_photoshop_blueprint.md  §Phase 3 (Headless DX12 Environment)
//
// This module:
//   1. Asserts the NVIDIA ALLOW_UNSIGNED registry override keys.
//   2. Enumerates DXGI 1.6 adapters with HIGH_PERFORMANCE preference,
//      selecting the first discrete NVIDIA GPU (vendor 0x10DE).
//   3. Creates a D3D12 device, DIRECT command queue, command allocator,
//      command list, and a CPU↔GPU synchronisation fence.
//   4. Does NOT create any swapchain — this is a pure headless context.
// =============================================================================

#include "DX12Context.h"

#include <d3d12sdklayers.h>  // ID3D12Debug
#include <cassert>
#include <cstdio>

// Link-time dependencies declared in CMakeLists.txt:
//   d3d12.lib, dxgi.lib, dxguid.lib, Advapi32.lib
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "Advapi32.lib")

namespace Gaslightr {

// ─────────────────────────────────────────────────────────────────────────────
// NVIDIA VENDOR ID — used to filter DXGI adapters
// ─────────────────────────────────────────────────────────────────────────────
static constexpr UINT NVIDIA_VENDOR_ID = 0x10DE;

// ─────────────────────────────────────────────────────────────────────────────
// Internal helper:  set a single DWORD registry value.
// Returns S_OK on success, or the LSTATUS wrapped as HRESULT.
// ─────────────────────────────────────────────────────────────────────────────
static HRESULT SetRegistryDword(
    HKEY        hRoot,
    const wchar_t* subKey,
    const wchar_t* valueName,
    DWORD       data) noexcept
{
    HKEY hKey = nullptr;
    LSTATUS status = ::RegCreateKeyExW(
        hRoot,
        subKey,
        0,          // reserved
        nullptr,    // class
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | KEY_WOW64_64KEY,
        nullptr,    // security attributes
        &hKey,
        nullptr     // disposition
    );
    if (status != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(status);
    }

    status = ::RegSetValueExW(
        hKey,
        valueName,
        0,          // reserved
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&data),
        sizeof(data)
    );
    ::RegCloseKey(hKey);

    return (status == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(status);
}

// ═════════════════════════════════════════════════════════════════════════════
// AssertRegistryBypass
// ═════════════════════════════════════════════════════════════════════════════
//
//  Per the DLSS Programming Guide (ref 26 in architecture.md) and the
//  community wrapper conventions (OptiScaler, DLSS5-Feeder):
//
//  The NVIDIA driver validates the digital signature of any process
//  that calls NVSDK_NGX_D3D12_Init.  For locally compiled unsigned
//  binaries (like our Photoshop plugin), we must set developer override
//  keys in the Windows Registry BEFORE initialising the NGX context.
//
//  Three keys are written:
//
//    HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore
//        AllowUnsignedBinaries       = 1   (DWORD)
//        OverrideFeatureDenial       = 1   (DWORD)
//
//    HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm
//        DisableNgxSignatureChecks   = 1   (DWORD)
//
//  SECURITY NOTE:  This requires the host process to be running with
//  Administrator privileges (elevated).  If the writes fail, the
//  function returns a descriptive DX12Result::Fail so the caller can
//  surface a user-visible error rather than silently crashing.
//
// ═════════════════════════════════════════════════════════════════════════════
DX12Result DX12Context::AssertRegistryBypass()
{
    // ── Key 1 & 2:  NGXCore developer overrides ──────────────────────────
    static constexpr const wchar_t* kNGXCorePath =
        L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore";

    HRESULT hr = SetRegistryDword(
        HKEY_LOCAL_MACHINE, kNGXCorePath,
        L"AllowUnsignedBinaries", 1);
    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"Failed to write AllowUnsignedBinaries to HKLM\\"
            L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore.  "
            L"Ensure the process is running as Administrator.");
    }

    hr = SetRegistryDword(
        HKEY_LOCAL_MACHINE, kNGXCorePath,
        L"OverrideFeatureDenial", 1);
    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"Failed to write OverrideFeatureDenial to NGXCore.");
    }

    // ── Key 3:  Kernel-mode driver telemetry bypass ──────────────────────
    static constexpr const wchar_t* kNvlddmkmPath =
        L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm";

    hr = SetRegistryDword(
        HKEY_LOCAL_MACHINE, kNvlddmkmPath,
        L"DisableNgxSignatureChecks", 1);
    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"Failed to write DisableNgxSignatureChecks to nvlddmkm.  "
            L"Ensure the process is running as Administrator.");
    }

    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// InitializeHeadless
// ═════════════════════════════════════════════════════════════════════════════
DX12Result DX12Context::InitializeHeadless()
{
    if (m_initialised) {
        return DX12Result::Fail(E_UNEXPECTED,
            L"DX12Context::InitializeHeadless called twice.");
    }

    HRESULT hr = S_OK;

    // ── Step 0: Enable the D3D12 debug layer in debug builds ─────────────
    //    Reference: photoshop-dlssnr-blueprint.md §Component B
#ifdef _DEBUG
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();

            // Optionally enable GPU-based validation (very slow, but
            // catches mismatched resource states at the hardware level).
            Microsoft::WRL::ComPtr<ID3D12Debug1> debugController1;
            if (SUCCEEDED(debugController.As(&debugController1))) {
                debugController1->SetEnableGPUBasedValidation(FALSE);
                // Set to TRUE only when hunting phantom device-removal
                // errors.  The perf cost is extreme.
            }
        }
    }
#endif

    // ── Step 1: Create a DXGI 1.6 Factory ────────────────────────────────
    //    IDXGIFactory6 gives us EnumAdapterByGpuPreference, which lets
    //    us request the HIGH_PERFORMANCE adapter directly instead of
    //    iterating and comparing DXGI_ADAPTER_DESC manually.
    {
        UINT factoryFlags = 0;
#ifdef _DEBUG
        factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
        hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
        if (FAILED(hr)) {
            return DX12Result::Fail(hr,
                L"CreateDXGIFactory2 failed.  Is the Windows SDK installed?");
        }
    }

    // ── Step 2: Enumerate adapters — select discrete NVIDIA GPU ──────────
    //
    //    Strategy:
    //      1.  Ask the factory for the adapter with the best GPU perf.
    //      2.  Walk the list; pick the first one whose VendorId == 0x10DE.
    //      3.  If no NVIDIA adapter is found, fall back to the first
    //          high-performance adapter (could be AMD/Intel — useful for
    //          build-testing, though NGX will reject it at init time).
    //
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidateAdapter;
        bool foundNvidia = false;

        for (UINT adapterIndex = 0;
             SUCCEEDED(m_factory->EnumAdapterByGpuPreference(
                 adapterIndex,
                 DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                 IID_PPV_ARGS(&candidateAdapter)));
             ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc{};
            candidateAdapter->GetDesc1(&desc);

            // Skip software / WARP adapters.
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                candidateAdapter.Reset();
                continue;
            }

            // Verify the adapter can create a D3D12 device at FL 12_0
            // without actually creating one (nullptr device).
            if (FAILED(D3D12CreateDevice(
                    candidateAdapter.Get(),
                    D3D_FEATURE_LEVEL_12_0,
                    __uuidof(ID3D12Device),
                    nullptr)))
            {
                candidateAdapter.Reset();
                continue;
            }

            // Prefer NVIDIA.
            if (desc.VendorId == NVIDIA_VENDOR_ID) {
                m_adapter = candidateAdapter;
                foundNvidia = true;

                wchar_t msg[512];
                swprintf_s(msg,
                    L"[Gaslightr] Selected NVIDIA adapter: %s  "
                    L"(Dedicated VRAM: %zu MB)",
                    desc.Description,
                    desc.DedicatedVideoMemory / (1024 * 1024));
                OutputDebugStringW(msg);
                OutputDebugStringW(L"\n");
                break;
            }

            // Hold onto the first valid non-NVIDIA adapter as fallback.
            if (!m_adapter) {
                m_adapter = candidateAdapter;
            }
            candidateAdapter.Reset();
        }

        if (!m_adapter) {
            return DX12Result::Fail(DXGI_ERROR_NOT_FOUND,
                L"No D3D12-capable GPU adapter found on this system.");
        }
        if (!foundNvidia) {
            OutputDebugStringW(
                L"[Gaslightr] WARNING: No NVIDIA adapter found.  "
                L"Falling back to first high-performance adapter.  "
                L"NGX initialisation will fail without NVIDIA hardware.\n");
        }
    }

    // ── Step 3: Create the D3D12 Device ──────────────────────────────────
    //    Feature level 12_0 is the minimum required by the NGX DLSS 5
    //    runtime.  We do NOT need 12_1/12_2 features for the headless
    //    evaluation pipeline.
    hr = D3D12CreateDevice(
        m_adapter.Get(),
        D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"D3D12CreateDevice failed on the selected adapter.");
    }

    // ── Step 4: Create the DIRECT command queue ──────────────────────────
    //    Reference: photoshop-dlssnr-blueprint.md §Component B
    //    "D3D12_COMMAND_LIST_TYPE_DIRECT queue and an associated
    //     Command Allocator/List."
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        hr = m_device->CreateCommandQueue(
            &queueDesc, IID_PPV_ARGS(&m_commandQueue));
        if (FAILED(hr)) {
            return DX12Result::Fail(hr,
                L"Failed to create D3D12 DIRECT command queue.");
        }
    }

    // ── Step 5: Create the command allocator ─────────────────────────────
    hr = m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator));
    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"Failed to create D3D12 command allocator.");
    }

    // ── Step 6: Create the command list (initially closed) ───────────────
    //    We create it in the CLOSED state.  Each render pass will call
    //    Reset() on the allocator + command list before recording.
    hr = m_device->CreateCommandList(
        0,                                  // node mask
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(),
        nullptr,                            // initial PSO — none
        IID_PPV_ARGS(&m_commandList));
    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"Failed to create D3D12 command list.");
    }
    // Immediately close it; callers will Reset() before recording.
    m_commandList->Close();

    // ── Step 7: Create the CPU↔GPU fence ─────────────────────────────────
    //    Used by WaitForGPU() to block the CPU until the command queue
    //    has drained all submitted work — critical for the temporal
    //    warmup loop where each frame must complete before the next
    //    is recorded (ref: PipelineSimulator.cpp).
    m_fenceValue = 0;
    hr = m_device->CreateFence(
        m_fenceValue,
        D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"Failed to create D3D12 fence.");
    }

    m_fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        return DX12Result::Fail(
            HRESULT_FROM_WIN32(::GetLastError()),
            L"Failed to create fence event handle.");
    }

    m_initialised = true;
    OutputDebugStringW(L"[Gaslightr] Headless DX12 context initialised.\n");
    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// WaitForGPU
// ═════════════════════════════════════════════════════════════════════════════
//
//  Signals the fence on the command queue with the next value, then
//  blocks the calling thread until the GPU reaches that value.
//
//  This is the synchronisation primitive used by the temporal warmup
//  loop (PipelineSimulator) to ensure each DLSS evaluation pass
//  completes before the next frame's command list is recorded.
//
DX12Result DX12Context::WaitForGPU()
{
    if (!m_fence || !m_fenceEvent) {
        return DX12Result::Fail(E_UNEXPECTED,
            L"WaitForGPU called before fence was created.");
    }

    const uint64_t targetValue = ++m_fenceValue;

    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), targetValue);
    if (FAILED(hr)) {
        return DX12Result::Fail(hr,
            L"ID3D12CommandQueue::Signal failed.");
    }

    if (m_fence->GetCompletedValue() < targetValue) {
        hr = m_fence->SetEventOnCompletion(targetValue, m_fenceEvent);
        if (FAILED(hr)) {
            return DX12Result::Fail(hr,
                L"ID3D12Fence::SetEventOnCompletion failed.");
        }
        const DWORD waitResult = ::WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
        if (waitResult != WAIT_OBJECT_0) {
            return DX12Result::Fail(HRESULT_FROM_WIN32(::GetLastError()), L"GPU fence wait failed.");
        }
    }

    const HRESULT removed = m_device->GetDeviceRemovedReason();
    if (FAILED(removed)) return DX12Result::Fail(removed, L"Graphics device was removed during processing.");
    return DX12Result::Success();
}

// ═════════════════════════════════════════════════════════════════════════════
// Shutdown
// ═════════════════════════════════════════════════════════════════════════════
//
//  Graceful teardown: flush the queue, close the fence event, then
//  release all COM objects.  ComPtr destructors handle Release() calls.
//
//  Reference: architecture.md §Phase 4
//    "Ensure graceful cleanup of all DX12 resources … during the
//     teardown sequence.  Failure to manually release these handles
//     results in permanent VRAM memory leaks."
//
void DX12Context::Shutdown()
{
    if (!m_initialised) return;

    // Drain all pending GPU work.
    WaitForGPU();

    if (m_fenceEvent) {
        ::CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    // ComPtr Reset() calls Release() and nulls the pointer.
    // Order: command list → allocator → queue → device → adapter → factory.
    m_commandList.Reset();
    m_commandAllocator.Reset();
    m_commandQueue.Reset();
    m_fence.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_factory.Reset();

    m_initialised = false;
    OutputDebugStringW(L"[Gaslightr] DX12 context shut down.\n");
}

// ═════════════════════════════════════════════════════════════════════════════
// Destructor
// ═════════════════════════════════════════════════════════════════════════════
DX12Context::~DX12Context()
{
    Shutdown();
}

} // namespace Gaslightr
