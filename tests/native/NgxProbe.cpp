#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <filesystem>

// =============================================================================
// Gaslightr — Isolated Standalone DLSS-NR Probe Target (GaslightrNgxProbe)
//
// Purpose: Probing vendor/nvidia/incoming/nvngx_dlssnr.dll in an isolated
// process outside Photoshop to verify real ABI entrypoints, architecture
// requirements, and feature compatibility on the RTX 4080 SUPER without
// endangering the Photoshop host process.
// =============================================================================

using Microsoft::WRL::ComPtr;

// Function pointer typedefs matching actual binary exports
using PFN_NVSDK_NGX_GetAPIVersion = uint32_t(__cdecl *)(void);
using PFN_NVSDK_NGX_GetApplicationId = uint32_t(__cdecl *)(void);
using PFN_NVSDK_NGX_GetGPUArchitecture = uint32_t(__cdecl *)(void);
using PFN_NVSDK_NGX_GetSnippetVersion = uint32_t(__cdecl *)(void);
using PFN_NVSDK_NGX_GetDriverVersionEx = uint32_t(__cdecl *)(uint32_t* pVersionArray, uint32_t count, uint32_t* pOutCount);

using PFN_NVSDK_NGX_D3D12_GetFeatureRequirements = uint32_t(__cdecl *)(
    IDXGIAdapter* pAdapter,
    const void* pFeatureDiscoveryInfo,
    void* pOutSupported
);

// Standard NGX Feature Discovery & Requirements struct layouts
#pragma pack(push, 8)
struct NGX_FeatureDiscoveryInfo {
    uint32_t FeatureID;
    const wchar_t* ApplicationDataPath;
};

struct NGX_FeatureRequirement {
    uint32_t FeatureSupported;      // 0 = Supported, nonzero = Not Supported
    uint32_t MinDriverVersionMajor;
    uint32_t MinDriverVersionMinor;
};
#pragma pack(pop)

// Dedicated SEH isolation helper: isolated in separate function so C++ unwinding does not collide
static uint32_t SafeCallFeatureRequirements(
    PFN_NVSDK_NGX_D3D12_GetFeatureRequirements fn,
    IDXGIAdapter* pAdapter,
    const void* pInfo,
    void* pReq,
    bool* pThrew,
    DWORD* pExcCode
) {
    *pThrew = false;
    *pExcCode = 0;
    __try {
        return fn(pAdapter, pInfo, pReq);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *pThrew = true;
        *pExcCode = GetExceptionCode();
        return 0xBAD00000;
    }
}

static std::wstring FindDllPath() {
    const std::vector<std::wstring> candidates = {
        L"vendor\\nvidia\\incoming\\nvngx_dlssnr.dll",
        L"..\\vendor\\nvidia\\incoming\\nvngx_dlssnr.dll",
        L"uxp-frontend\\win\\x64\\nvngx_dlssnr.dll",
        L"..\\uxp-frontend\\win\\x64\\nvngx_dlssnr.dll"
    };
    for (const auto& path : candidates) {
        if (std::filesystem::exists(path)) {
            return std::filesystem::absolute(path).wstring();
        }
    }
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const auto dir = std::filesystem::path(exePath).parent_path();
    for (const auto& rel : { L"..\\..\\vendor\\nvidia\\incoming\\nvngx_dlssnr.dll", L"nvngx_dlssnr.dll" }) {
        const auto p = dir / rel;
        if (std::filesystem::exists(p)) return std::filesystem::absolute(p).wstring();
    }
    return L"vendor\\nvidia\\incoming\\nvngx_dlssnr.dll";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << " Gaslightr Standalone NVIDIA NGX DLSS-NR Probe Target\n";
    std::cout << " Host Hardware: NVIDIA GeForce RTX 4080 SUPER (Ada Lovelace)\n";
    std::cout << "========================================================\n\n";

    // 1. Initialize Direct3D 12 and enumerate DXGI Adapter
    std::cout << "[Step 1] Initializing D3D12 and DXGI on local hardware...\n";
    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::cerr << "[-] CreateDXGIFactory2 failed: 0x" << std::hex << hr << "\n";
        return 1;
    }

    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapterDesc{};
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        adapter->GetDesc1(&adapterDesc);
        if (adapterDesc.VendorId == 0x10DE) { // NVIDIA
            break;
        }
        adapter.Reset();
    }

    if (!adapter) {
        std::cerr << "[-] No NVIDIA discrete adapter found.\n";
        return 1;
    }

    std::wcout << L"[+] Selected Adapter: " << adapterDesc.Description << L"\n";
    std::cout << "    Vendor ID: 0x" << std::hex << adapterDesc.VendorId
              << " | Device ID: 0x" << adapterDesc.DeviceId << std::dec << "\n";
    std::cout << "    Dedicated Video Memory: " << (adapterDesc.DedicatedVideoMemory / (1024 * 1024)) << " MB\n";

    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        std::cerr << "[-] D3D12CreateDevice failed: 0x" << std::hex << hr << "\n";
        return 1;
    }
    std::cout << "[+] D3D12 Device initialized successfully at Feature Level 12.0.\n\n";

    // 2. Dynamically load vendor/nvidia/incoming/nvngx_dlssnr.dll
    std::cout << "[Step 2] Dynamically loading nvngx_dlssnr.dll...\n";
    const std::wstring dllPath = FindDllPath();
    std::wcout << L"[i] Target binary path: " << dllPath << L"\n";

    HMODULE hNgx = LoadLibraryW(dllPath.c_str());
    if (!hNgx) {
        const DWORD err = GetLastError();
        std::cerr << "[-] LoadLibraryW failed with error code: " << err << "\n";
        return 1;
    }
    std::cout << "[+] nvngx_dlssnr.dll loaded into isolated probe process at 0x"
              << std::hex << reinterpret_cast<uintptr_t>(hNgx) << std::dec << "\n\n";

    // 3. Resolve and query metadata exports
    std::cout << "[Step 3] Querying core metadata exports...\n";

    auto pfnGetAPIVersion = reinterpret_cast<PFN_NVSDK_NGX_GetAPIVersion>(
        GetProcAddress(hNgx, "NVSDK_NGX_GetAPIVersion"));
    if (pfnGetAPIVersion) {
        const uint32_t apiVer = pfnGetAPIVersion();
        std::cout << "[+] NVSDK_NGX_GetAPIVersion: 0x" << std::hex << apiVer
                  << " (" << std::dec << apiVer << ")\n";
    } else {
        std::cout << "[-] NVSDK_NGX_GetAPIVersion not found.\n";
    }

    auto pfnGetAppId = reinterpret_cast<PFN_NVSDK_NGX_GetApplicationId>(
        GetProcAddress(hNgx, "NVSDK_NGX_GetApplicationId"));
    if (pfnGetAppId) {
        const uint32_t appId = pfnGetAppId();
        std::cout << "[+] NVSDK_NGX_GetApplicationId: 0x" << std::hex << appId
                  << " (" << std::dec << appId << ")\n";
    } else {
        std::cout << "[-] NVSDK_NGX_GetApplicationId not found.\n";
    }

    auto pfnGetGPUArch = reinterpret_cast<PFN_NVSDK_NGX_GetGPUArchitecture>(
        GetProcAddress(hNgx, "NVSDK_NGX_GetGPUArchitecture"));
    if (pfnGetGPUArch) {
        const uint32_t arch = pfnGetGPUArch();
        std::cout << "[+] NVSDK_NGX_GetGPUArchitecture: 0x" << std::hex << arch
                  << " (Ada Lovelace enum: " << (arch == 0x1B0 ? "MATCH" : "MISMATCH") << ")\n";
    } else {
        std::cout << "[-] NVSDK_NGX_GetGPUArchitecture not found.\n";
    }

    auto pfnGetSnippetVersion = reinterpret_cast<PFN_NVSDK_NGX_GetSnippetVersion>(
        GetProcAddress(hNgx, "NVSDK_NGX_GetSnippetVersion"));
    if (pfnGetSnippetVersion) {
        const uint32_t snippetVer = pfnGetSnippetVersion();
        const uint32_t major = (snippetVer >> 16) & 0xFFFF;
        const uint32_t minor = (snippetVer >> 8) & 0xFF;
        const uint32_t patch = snippetVer & 0xFF;
        std::cout << "[+] NVSDK_NGX_GetSnippetVersion: 0x" << std::hex << snippetVer
                  << " (Version " << std::dec << major << "." << minor << "." << patch << ")\n";
    } else {
        std::cout << "[-] NVSDK_NGX_GetSnippetVersion not found.\n";
    }

    auto pfnGetDriverVer = reinterpret_cast<PFN_NVSDK_NGX_GetDriverVersionEx>(
        GetProcAddress(hNgx, "NVSDK_NGX_GetDriverVersionEx"));
    if (pfnGetDriverVer) {
        uint32_t verArray[4]{};
        uint32_t count = 0;
        const uint32_t res = pfnGetDriverVer(verArray, 4, &count);
        std::cout << "[+] NVSDK_NGX_GetDriverVersionEx result: 0x" << std::hex << res << std::dec
                  << " | Required Driver: " << verArray[0] << "." << verArray[1] << " (Count: " << count << ")\n";
    } else {
        std::cout << "[-] NVSDK_NGX_GetDriverVersionEx not found.\n";
    }

    // 4. Probe NVSDK_NGX_D3D12_GetFeatureRequirements
    std::cout << "\n[Step 4] Probing NVSDK_NGX_D3D12_GetFeatureRequirements on RTX 4080 SUPER...\n";
    auto pfnGetFeatureReq = reinterpret_cast<PFN_NVSDK_NGX_D3D12_GetFeatureRequirements>(
        GetProcAddress(hNgx, "NVSDK_NGX_D3D12_GetFeatureRequirements"));

    if (pfnGetFeatureReq) {
        std::cout << "[+] NVSDK_NGX_D3D12_GetFeatureRequirements resolved at 0x"
                  << std::hex << reinterpret_cast<uintptr_t>(pfnGetFeatureReq) << std::dec << "\n";

        // Test Feature ID 18 (DLSS-NR / RayReconstruction) and other IDs
        const uint32_t featureIDs[] = { 18, 1, 2, 3 };
        for (uint32_t fId : featureIDs) {
            NGX_FeatureDiscoveryInfo info{};
            info.FeatureID = fId;
            info.ApplicationDataPath = L"";

            NGX_FeatureRequirement req{};
            req.FeatureSupported = 0xFFFFFFFF;

            bool threw = false;
            DWORD excCode = 0;
            const uint32_t result = SafeCallFeatureRequirements(pfnGetFeatureReq, adapter.Get(), &info, &req, &threw, &excCode);

            if (threw) {
                std::cout << "    [!] Feature ID " << fId << ": SEH Exception caught: 0x"
                          << std::hex << excCode << std::dec << "\n";
            } else {
                std::cout << "    [+] Feature ID " << fId << ": NVSDK_NGX_Result = 0x"
                          << std::hex << result << std::dec
                          << " (Supported=" << req.FeatureSupported
                          << ", MinDriver=" << req.MinDriverVersionMajor << "." << req.MinDriverVersionMinor << ")\n";
            }
        }
    } else {
        std::cout << "[-] NVSDK_NGX_D3D12_GetFeatureRequirements not found.\n";
    }

    // 5. Clean teardown
    std::cout << "\n[Step 5] Teardown & Findings Summary...\n";
    FreeLibrary(hNgx);
    std::cout << "[+] Probe completed safely in isolation. Host environment is protected.\n";
    std::cout << "========================================================\n";

    return 0;
}
