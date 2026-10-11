#pragma once
// =============================================================================
// Gaslightr — DX12Context.h
// Headless Direct3D 12 Device, Command Queue, and Command Allocator.
// No swapchain.  No window.  Pure compute-path GPU residency.
// =============================================================================

#include <windows.h>
#include <wrl/client.h>    // Microsoft::WRL::ComPtr

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <string>

namespace Gaslightr {

// ── Result type for operations that can fail with diagnostic info ────────────
struct DX12Result {
    bool     ok      = false;
    HRESULT  hr      = S_OK;
    std::wstring detail;

    explicit operator bool() const noexcept { return ok; }

    static DX12Result Success() { return { true, S_OK, L"" }; }
    static DX12Result Fail(HRESULT h, const std::wstring& msg) {
        return { false, h, msg };
    }
};

// ── Headless DX12 context ────────────────────────────────────────────────────
//
//  Initialises a D3D12 device bound to the highest-performance discrete NVIDIA
//  adapter.  Creates a DIRECT command queue and command allocator for issuing
//  GPU work (texture uploads, NGX evaluation calls, readbacks).
//
//  IMPORTANT:  Before this context is created, the caller MUST invoke
//  AssertRegistryBypass() to set the NVIDIA ALLOW_UNSIGNED override keys.
//  Failure to do so will cause NVSDK_NGX_D3D12_Init to reject unsigned
//  binaries with a FeatureDenial error.
//
class DX12Context {
public:
    DX12Context()  = default;
    ~DX12Context();

    // Non-copyable, non-movable (COM pointers are tied to this instance).
    DX12Context(const DX12Context&)            = delete;
    DX12Context& operator=(const DX12Context&) = delete;
    DX12Context(DX12Context&&)                 = delete;
    DX12Context& operator=(DX12Context&&)      = delete;

    // ── Registry bypass ──────────────────────────────────────────────────
    //
    //  Programmatically asserts the NVIDIA NGX developer override keys
    //  that disable the driver-level cryptographic signature check on
    //  unsigned local binaries.
    //
    //  Registry path:
    //    HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore
    //      AllowUnsignedBinaries     REG_DWORD  1
    //      OverrideFeatureDenial     REG_DWORD  1
    //
    //    HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm
    //      DisableNgxSignatureChecks REG_DWORD  1
    //
    //  Returns DX12Result::Fail if any key cannot be written (typically
    //  means the process lacks Administrator elevation).
    //
    static DX12Result AssertRegistryBypass();

    // ── Device initialisation ────────────────────────────────────────────
    //
    //  1.  Enumerates DXGI adapters via IDXGIFactory6 with the
    //      HIGH_PERFORMANCE preference, selecting the first discrete
    //      NVIDIA GPU (vendor ID 0x10DE).
    //  2.  Creates a D3D12 device at feature level 12_0.
    //  3.  Creates a DIRECT command queue and command allocator.
    //  4.  Creates a GPU synchronisation fence for WaitForGPU().
    //
    //  Optionally enables the D3D12 debug layer in _DEBUG builds.
    //
    DX12Result InitializeHeadless();

    // ── GPU synchronisation ──────────────────────────────────────────────
    //  Signals the fence on the queue and blocks until the GPU drains
    //  all previously submitted command lists.
    DX12Result WaitForGPU();

    // ── Teardown ─────────────────────────────────────────────────────────
    //  Flushes the queue, then releases all COM objects in reverse order.
    void Shutdown();

    // ── Accessors (non-owning raw pointers for downstream subsystems) ───
    ID3D12Device*              GetDevice()           const noexcept { return m_device.Get(); }
    ID3D12CommandQueue*        GetCommandQueue()      const noexcept { return m_commandQueue.Get(); }
    ID3D12CommandAllocator*    GetCommandAllocator()  const noexcept { return m_commandAllocator.Get(); }
    ID3D12GraphicsCommandList* GetCommandList()       const noexcept { return m_commandList.Get(); }
    IDXGIAdapter1*             GetAdapter()           const noexcept { return m_adapter.Get(); }

private:
    // ── DXGI factory & adapter ───────────────────────────────────────────
    Microsoft::WRL::ComPtr<IDXGIFactory6>            m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>            m_adapter;

    // ── D3D12 core objects ───────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12Device>             m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        m_commandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // ── Fence for CPU ↔ GPU synchronisation ──────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12Fence>              m_fence;
    HANDLE                                            m_fenceEvent = nullptr;
    uint64_t                                          m_fenceValue = 0;

    bool m_initialised = false;
};

} // namespace Gaslightr
