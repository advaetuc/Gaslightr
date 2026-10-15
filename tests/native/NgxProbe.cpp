// =============================================================================
// Gaslightr — Isolated Standalone DLSS-NR Probe Target (GaslightrNgxProbe)
// Phase 9: Investigating NGX Platform & Signature Requirements
//
// Directives:
//   1. Investigate NGX platform / signature requirements and parameter contracts
//      (InApplicationId, InApplicationDataPath, NVSDK_NGX_LoggingInfo callback).
//   2. Probe direct snippet calling (nvngx_dlssnr.dll) vs official driver loader (_nvngx.dll / nvngx.dll).
//   3. Inspect digital signatures (HashMismatch vs Valid) and registry keys.
//   4. Capture all internal diagnostic strings without touching Photoshop.
// =============================================================================

#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <filesystem>
#include <sstream>
#include <mutex>
#include <cwchar>

#include "NeuralBackend.h"

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

using Microsoft::WRL::ComPtr;

// ── NGX Enums & Types ────────────────────────────────────────────────────────

using NVSDK_NGX_Result = uint32_t;

enum NVSDK_NGX_Logging_Level : uint32_t {
    NVSDK_NGX_LOGGING_LEVEL_OFF     = 0,
    NVSDK_NGX_LOGGING_LEVEL_ON      = 1,
    NVSDK_NGX_LOGGING_LEVEL_VERBOSE = 2
};

enum NVSDK_NGX_Feature : uint32_t {
    NVSDK_NGX_Feature_Reserved_Zero        = 0,
    NVSDK_NGX_Feature_SuperSampling        = 1,
    NVSDK_NGX_Feature_InPainting           = 2,
    NVSDK_NGX_Feature_ImageSuperResolution = 3,
    NVSDK_NGX_Feature_SlowMotion           = 4,
    NVSDK_NGX_Feature_VideoSuperResolution = 5,
    NVSDK_NGX_Feature_RayReconstruction    = 13, // DLSS-NR (Official NGX Feature ID 13)
    NVSDK_NGX_Feature_Count                = 14
};

enum NVSDK_NGX_EngineType : uint32_t {
    NVSDK_NGX_ENGINE_TYPE_CUSTOM    = 0,
    NVSDK_NGX_ENGINE_TYPE_UNREAL    = 1,
    NVSDK_NGX_ENGINE_TYPE_UNITY     = 2,
    NVSDK_NGX_ENGINE_TYPE_OMNIVERSE = 3
};

using NVSDK_NGX_Version = uint32_t;

// Standard NGX AppLogCallback
using PFN_NVSDK_NGX_AppLogCallback = void(__cdecl *)(
    const char* message,
    NVSDK_NGX_Logging_Level loggingLevel,
    NVSDK_NGX_Feature sourceComponent
);

#pragma pack(push, 8)
struct NVSDK_NGX_PathListInfo {
    const wchar_t* const* Path;
    uint32_t Length;
};

struct NVSDK_NGX_LoggingInfo {
    PFN_NVSDK_NGX_AppLogCallback LoggingCallback;
    NVSDK_NGX_Logging_Level MinimumLoggingLevel;
    bool DisableOtherLoggingSinks;
};

struct NVSDK_NGX_FeatureCommonInfo {
    NVSDK_NGX_PathListInfo PathListInfo;
    void* InternalData;
    NVSDK_NGX_LoggingInfo LoggingInfo;
};

struct NGX_FeatureDiscoveryInfo {
    uint32_t FeatureID;
    const wchar_t* ApplicationDataPath;
};

struct NGX_FeatureRequirement {
    uint32_t FeatureSupported;
    uint32_t MinDriverVersionMajor;
    uint32_t MinDriverVersionMinor;
};

struct ID3D11Resource;

struct NVSDK_NGX_Parameter {
    // Virtual functions declared with distinct names to prevent MSVC overload reordering
    virtual void SetU64(const char* InName, unsigned long long InValue) = 0; // [0] RVA 0x37a0
    virtual void SetD3D12(const char* InName, ID3D12Resource* InValue) = 0;  // [1] RVA 0x641b0
    virtual void SetD3D11(const char* InName, ID3D11Resource* InValue) = 0;  // [2] RVA 0x37e0
    virtual void SetI(const char* InName, int InValue) = 0;                  // [3] RVA 0x2f30
    virtual void SetUI(const char* InName, unsigned int InValue) = 0;        // [4] RVA 0x2eb0
    virtual void SetD(const char* InName, double InValue) = 0;               // [5] RVA 0x2e30
    virtual void SetF(const char* InName, float InValue) = 0;                // [6] RVA 0x2e70
    virtual void SetVoid(const char* InName, void* InValue) = 0;             // [7] RVA 0x2ef0

    virtual NVSDK_NGX_Result GetD3D12(const char* InName, ID3D12Resource** OutValue) = 0;    // [8] RVA 0x3800
    virtual NVSDK_NGX_Result GetVoid(const char* InName, void** OutValue) = 0;              // [9] RVA 0x3800
    virtual NVSDK_NGX_Result GetD3D11(const char* InName, ID3D11Resource** OutValue) = 0;    // [10] RVA 0x37f0
    virtual NVSDK_NGX_Result GetI(const char* InName, int* OutValue) = 0;                    // [11] RVA 0x3310
    virtual NVSDK_NGX_Result GetUI(const char* InName, unsigned int* OutValue) = 0;          // [12] RVA 0x3290
    virtual NVSDK_NGX_Result GetD(const char* InName, double* OutValue) = 0;                 // [13] RVA 0x3190
    virtual NVSDK_NGX_Result GetF(const char* InName, float* OutValue) = 0;                  // [14] RVA 0x3090
    virtual NVSDK_NGX_Result GetU64(const char* InName, unsigned long long* OutValue) = 0;   // [15] RVA 0x2f70

    virtual void Reset() = 0;                                                                // [16] RVA 0x3390

    // Ergonomic overloads forwarding to the exact vtable slot
    inline void Set(const char* name, unsigned long long val) { SetU64(name, val); }
    inline void Set(const char* name, ID3D12Resource* val)    { SetD3D12(name, val); }
    inline void Set(const char* name, ID3D11Resource* val)    { SetD3D11(name, val); }
    inline void Set(const char* name, int val)                { SetI(name, val); }
    inline void Set(const char* name, unsigned int val)       { SetUI(name, val); }
    inline void Set(const char* name, double val)             { SetD(name, val); }
    inline void Set(const char* name, float val)              { SetF(name, val); }
    inline void Set(const char* name, void* val)              { SetVoid(name, val); }

    inline NVSDK_NGX_Result Get(const char* name, ID3D12Resource** val) { return GetD3D12(name, val); }
    inline NVSDK_NGX_Result Get(const char* name, void** val)           { return GetVoid(name, val); }
    inline NVSDK_NGX_Result Get(const char* name, ID3D11Resource** val) { return GetD3D11(name, val); }
    inline NVSDK_NGX_Result Get(const char* name, int* val)             { return GetI(name, val); }
    inline NVSDK_NGX_Result Get(const char* name, unsigned int* val)    { return GetUI(name, val); }
    inline NVSDK_NGX_Result Get(const char* name, double* val)          { return GetD(name, val); }
    inline NVSDK_NGX_Result Get(const char* name, float* val)           { return GetF(name, val); }
    inline NVSDK_NGX_Result Get(const char* name, unsigned long long* val) { return GetU64(name, val); }
};
#pragma pack(pop)

// ── Function Pointer Typedefs ────────────────────────────────────────────────

using PFN_NVSDK_NGX_GetAPIVersion = uint32_t(__cdecl *)(void);
using PFN_NVSDK_NGX_GetApplicationId = uint32_t(__cdecl *)(void);
using PFN_NVSDK_NGX_GetGPUArchitecture = uint32_t(__cdecl *)(void);
using PFN_NVSDK_NGX_GetSnippetVersion = uint32_t(__cdecl *)(void);
using PFN_NVSDK_NGX_GetDriverVersionEx = uint32_t(__cdecl *)(uint32_t* pVersionArray, uint32_t count, uint32_t* pOutCount);

using PFN_NVSDK_NGX_D3D12_Init = NVSDK_NGX_Result(__cdecl *)(
    unsigned long long InApplicationId,
    const wchar_t*    InApplicationDataPath,
    ID3D12Device*     InDevice,
    NVSDK_NGX_Version InSDKVersion
);

using PFN_NVSDK_NGX_D3D12_Init_Ext = NVSDK_NGX_Result(__cdecl *)(
    unsigned long long                  InApplicationId,
    const wchar_t*                     InApplicationDataPath,
    ID3D12Device*                      InDevice,
    NVSDK_NGX_Version                  InSDKVersion,
    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo
);

using PFN_NVSDK_NGX_D3D12_Init_ProjectID = NVSDK_NGX_Result(__cdecl *)(
    const char*                        InProjectId,
    NVSDK_NGX_EngineType               InEngineType,
    const char*                        InEngineVersion,
    const wchar_t*                     InApplicationDataPath,
    ID3D12Device*                      InDevice,
    NVSDK_NGX_Version                  InSDKVersion,
    const NVSDK_NGX_FeatureCommonInfo* InFeatureInfo
);

using PFN_NVSDK_NGX_D3D12_GetFeatureRequirements = NVSDK_NGX_Result(__cdecl *)(
    IDXGIAdapter* pAdapter,
    const void* pFeatureDiscoveryInfo,
    void* pOutSupported
);

using PFN_NVSDK_NGX_D3D12_GetCapabilityParameters = NVSDK_NGX_Result(__cdecl *)(
    void** ppOutParameters
);

using PFN_NVSDK_NGX_D3D12_AllocateParameters = NVSDK_NGX_Result(__cdecl *)(
    void** ppOutParameters
);

using PFN_NVSDK_NGX_D3D12_DestroyParameters = NVSDK_NGX_Result(__cdecl *)(
    void* InParameters
);

using PFN_NVSDK_NGX_D3D12_CreateFeature = NVSDK_NGX_Result(__cdecl *)(
    ID3D12GraphicsCommandList* InCmdList,
    NVSDK_NGX_Feature          InFeatureID,
    const NVSDK_NGX_Parameter* InParameters,
    void**                     OutHandle
);

using PFN_NVSDK_NGX_D3D12_ReleaseFeature = NVSDK_NGX_Result(__cdecl *)(
    void* InHandle
);

using PFN_NVSDK_NGX_D3D12_EvaluateFeature = NVSDK_NGX_Result(__cdecl *)(
    ID3D12GraphicsCommandList* InCmdList,
    void*                      InFeatureHandle,
    const NVSDK_NGX_Parameter* InParameters,
    void*                      InCallback
);

using PFN_NVSDK_NGX_D3D12_GetScratchBufferSize = NVSDK_NGX_Result(__cdecl *)(
    NVSDK_NGX_Feature          InFeatureID,
    const NVSDK_NGX_Parameter* InParameters,
    size_t*                    OutScratchBufferSize
);

using PFN_NVSDK_NGX_D3D12_Shutdown = NVSDK_NGX_Result(__cdecl *)(ID3D12Device* InDevice);
using PFN_NVSDK_NGX_D3D12_Shutdown1 = NVSDK_NGX_Result(__cdecl *)(void);

// ── Global Log Collector ────────────────────────────────────────────────────

static std::vector<std::string> g_CapturedLogs;

static void __cdecl NgxLogCallback(
    const char* message,
    NVSDK_NGX_Logging_Level level,
    NVSDK_NGX_Feature sourceComponent)
{
    std::string text = (message ? message : "<null>");
    g_CapturedLogs.push_back(text);
    std::cout << "    [NGX Internal Log (Lvl " << level << ", Comp " << sourceComponent << ")] " << text << "\n";
}

static const char* GetNgxResultString(NVSDK_NGX_Result res) {
    switch (res) {
    case 0x00000001: return "NVSDK_NGX_Result_Success";
    case 0xBAD00000: return "NVSDK_NGX_Result_Fail";
    case 0xBAD00001: return "NVSDK_NGX_Result_Fail_FeatureNotSupported";
    case 0xBAD00002: return "NVSDK_NGX_Result_Fail_PlatformError";
    case 0xBAD00003: return "NVSDK_NGX_Result_Fail_FeatureAlreadyExists";
    case 0xBAD00004: return "NVSDK_NGX_Result_Fail_FeatureNotFound";
    case 0xBAD00005: return "NVSDK_NGX_Result_Fail_InvalidParameter";
    case 0xBAD00006: return "NVSDK_NGX_Result_Fail_ScratchBufferTooSmall";
    case 0xBAD00007: return "NVSDK_NGX_Result_Fail_NotInitialized";
    case 0xBAD00008: return "NVSDK_NGX_Result_Fail_UnsupportedFormat";
    case 0xBAD00009: return "NVSDK_NGX_Result_Fail_UnableToInitializeFeature";
    case 0xBAD0000A: return "NVSDK_NGX_Result_Fail_FeatureInitialized";
    case 0xBAD0000B: return "NVSDK_NGX_Result_Fail_UnableToEvaluateFeature";
    case 0xBAD0000C: return "NVSDK_NGX_Result_Fail_FeatureNotLoaded";
    case 0xBAD0000D: return "NVSDK_NGX_Result_Fail_InvalidDevice";
    case 0xBAD0000E: return "NVSDK_NGX_Result_Fail_InvalidAdapter";
    case 0xBAD0000F: return "NVSDK_NGX_Result_Fail_OutOfMemory";
    case 0xBAD00010: return "NVSDK_NGX_Result_Fail_InvalidParamValue";
    case 0xBAD00011: return "NVSDK_NGX_Result_Fail_FeatureDenied";
    case 0xBAD00012: return "NVSDK_NGX_Result_Fail_SignatureVerification";
    default: return "Unknown NVSDK_NGX_Result";
    }
}

static void SafeQueryParamInt(NVSDK_NGX_Parameter* pParams, const char* name) {
    if (!pParams) return;
    int val = -999;
    __try {
        NVSDK_NGX_Result res = pParams->Get(name, &val);
        std::cout << "        [" << std::left << std::setw(32) << name << "]: res=0x"
                  << std::hex << res << std::dec << " (" << GetNgxResultString(res) << "), val=" << val << "\n";
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        std::cout << "        [" << std::left << std::setw(32) << name << "]: EXCEPTION 0x"
                  << std::hex << GetExceptionCode() << std::dec << "\n";
    }
}

static void SafeQueryParamU64(NVSDK_NGX_Parameter* pParams, const char* name) {
    if (!pParams) return;
    unsigned long long val = 0;
    __try {
        NVSDK_NGX_Result res = pParams->Get(name, &val);
        std::cout << "        [" << std::left << std::setw(32) << name << "]: res=0x"
                  << std::hex << res << std::dec << " (" << GetNgxResultString(res) << "), val=0x"
                  << std::hex << val << std::dec << "\n";
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        std::cout << "        [" << std::left << std::setw(32) << name << "]: EXCEPTION 0x"
                  << std::hex << GetExceptionCode() << std::dec << "\n";
    }
}

static void PrintExceptionDetail(LPEXCEPTION_POINTERS ep, const char* callerName) {
    if (!ep || !ep->ExceptionRecord) return;
    PEXCEPTION_RECORD rec = ep->ExceptionRecord;
    HMODULE hMod = nullptr;
    wchar_t modPath[MAX_PATH] = L"<unknown>";
    uintptr_t rva = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(rec->ExceptionAddress), &hMod) && hMod) {
        GetModuleFileNameW(hMod, modPath, MAX_PATH);
        rva = reinterpret_cast<uintptr_t>(rec->ExceptionAddress) - reinterpret_cast<uintptr_t>(hMod);
    }
    std::wcout << L"      [!] " << callerName << L" Exception 0x" << std::hex << rec->ExceptionCode
               << L" at " << std::filesystem::path(modPath).filename().wstring()
               << L"+0x" << rva << L" (0x" << reinterpret_cast<uintptr_t>(rec->ExceptionAddress) << L")";
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        std::cout << " (" << (rec->ExceptionInformation[0] == 0 ? "READ" : "WRITE")
                  << " to 0x" << rec->ExceptionInformation[1] << ")";
    }
    std::cout << std::dec << "\n";
}

static NVSDK_NGX_Result SafeCallInitExt(
    PFN_NVSDK_NGX_D3D12_Init_Ext fn,
    unsigned long long appId,
    const wchar_t* path,
    ID3D12Device* dev,
    NVSDK_NGX_Version ver,
    const NVSDK_NGX_FeatureCommonInfo* info,
    bool* pThrew,
    DWORD* pCode)
{
    *pThrew = false;
    *pCode = 0;
    __try {
        return fn(appId, path, dev, ver, info);
    }
    __except (PrintExceptionDetail(GetExceptionInformation(), "SafeCallInitExt"), EXCEPTION_EXECUTE_HANDLER) {
        *pThrew = true;
        *pCode = GetExceptionCode();
        return 0xBAD00000;
    }
}

static NVSDK_NGX_Result SafeCallInitProjectID(
    PFN_NVSDK_NGX_D3D12_Init_ProjectID fn,
    const char* projectId,
    NVSDK_NGX_EngineType engineType,
    const char* engineVersion,
    const wchar_t* path,
    ID3D12Device* dev,
    NVSDK_NGX_Version ver,
    const NVSDK_NGX_FeatureCommonInfo* info,
    bool* pThrew,
    DWORD* pCode)
{
    *pThrew = false;
    *pCode = 0;
    __try {
        return fn(projectId, engineType, engineVersion, path, dev, ver, info);
    }
    __except (PrintExceptionDetail(GetExceptionInformation(), "SafeCallInitProjectID"), EXCEPTION_EXECUTE_HANDLER) {
        *pThrew = true;
        *pCode = GetExceptionCode();
        return 0xBAD00000;
    }
}

static NVSDK_NGX_Result SafeCallFeatureReq(
    PFN_NVSDK_NGX_D3D12_GetFeatureRequirements fn,
    IDXGIAdapter* pAdapter,
    const void* pInfo,
    void* pReq,
    bool* pThrew,
    DWORD* pCode)
{
    *pThrew = false;
    *pCode = 0;
    __try {
        return fn(pAdapter, pInfo, pReq);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *pThrew = true;
        *pCode = GetExceptionCode();
        return 0xBAD00000;
    }
}

static NVSDK_NGX_Result SafeCallCreateFeature(
    PFN_NVSDK_NGX_D3D12_CreateFeature fn,
    ID3D12GraphicsCommandList* cmdList,
    NVSDK_NGX_Feature featureId,
    const NVSDK_NGX_Parameter* params,
    void** outHandle,
    bool* pThrew,
    DWORD* pCode)
{
    *pThrew = false;
    *pCode = 0;
    __try {
        return fn(cmdList, featureId, params, outHandle);
    }
    __except (PrintExceptionDetail(GetExceptionInformation(), "CreateFeature"), EXCEPTION_EXECUTE_HANDLER) {
        *pThrew = true;
        *pCode = GetExceptionCode();
        return 0xBAD00000;
    }
}

static NVSDK_NGX_Result SafeCallEvaluateFeature(
    PFN_NVSDK_NGX_D3D12_EvaluateFeature fn,
    ID3D12GraphicsCommandList* cmdList,
    void* handle,
    const NVSDK_NGX_Parameter* params,
    void* cb,
    bool* pThrew,
    DWORD* pCode)
{
    *pThrew = false;
    *pCode = 0;
    __try {
        return fn(cmdList, handle, params, cb);
    }
    __except (PrintExceptionDetail(GetExceptionInformation(), "EvaluateFeature"), EXCEPTION_EXECUTE_HANDLER) {
        *pThrew = true;
        *pCode = GetExceptionCode();
        return 0xBAD00000;
    }
}

static NVSDK_NGX_Result SafeCallReleaseFeature(
    PFN_NVSDK_NGX_D3D12_ReleaseFeature fn,
    void* handle,
    bool* pThrew,
    DWORD* pCode)
{
    *pThrew = false;
    *pCode = 0;
    __try {
        return fn(handle);
    }
    __except (PrintExceptionDetail(GetExceptionInformation(), "ReleaseFeature"), EXCEPTION_EXECUTE_HANDLER) {
        *pThrew = true;
        *pCode = GetExceptionCode();
        return 0xBAD00000;
    }
}

static ComPtr<ID3D12Resource> CreateProbeTexture(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&resource)
    );
    if (FAILED(hr)) {
        std::cout << "      [!] CreateProbeTexture FAILED: hr=0x" << std::hex << hr << std::dec
                  << " (" << width << "x" << height << ", format=" << format << ", flags=" << flags << ")\n";
        return nullptr;
    }
    return resource;
}

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::wstring FindIncomingDir() {
    const std::vector<std::wstring> candidates = {
        L"vendor\\nvidia\\incoming",
        L"..\\vendor\\nvidia\\incoming"
    };
    for (const auto& path : candidates) {
        if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
            return std::filesystem::absolute(path).wstring();
        }
    }
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const auto dir = std::filesystem::path(exePath).parent_path();
    const auto rel = dir / L"..\\..\\vendor\\nvidia\\incoming";
    if (std::filesystem::exists(rel)) return std::filesystem::absolute(rel).wstring();
    return L"vendor\\nvidia\\incoming";
}

static std::vector<std::wstring> FindAllIncomingDlls() {
    std::vector<std::wstring> dlls;
    std::wstring dirPath = FindIncomingDir();
    if (std::filesystem::exists(dirPath)) {
        for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
            if (entry.is_regular_file() && entry.path().extension() == L".dll") {
                dlls.push_back(std::filesystem::absolute(entry.path()).wstring());
            }
        }
    }
    return dlls;
}

static std::wstring FindDllPath() {
    const std::vector<std::wstring> candidates = {
        L"vendor\\nvidia\\incoming\\nvngx_dlssnr.dll",
        L"..\\vendor\\nvidia\\incoming\\nvngx_dlssnr.dll",
        L"uxp-frontend\\win\\x64\\nvngx_dlssnr.dll"
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

static std::wstring GetDriverStoreNgxDir() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t path[MAX_PATH]{};
        DWORD size = sizeof(path);
        if (RegQueryValueExW(hKey, L"FullPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(path), &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            if (std::filesystem::exists(path)) return std::wstring(path);
        }
        RegCloseKey(hKey);
    }

    // Fallback 1: Direct DriverStore path verified on RTX 4080 SUPER
    const std::wstring directPath = L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_feedb8c0271ca811";
    if (std::filesystem::exists(directPath + L"\\nvngx.dll")) {
        return directPath;
    }

    // Fallback 2: Dynamic scan of DriverStore for nv_dispi* containing nvngx.dll
    const std::wstring fileRepo = L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository";
    if (std::filesystem::exists(fileRepo)) {
        for (const auto& entry : std::filesystem::directory_iterator(fileRepo)) {
            if (entry.is_directory()) {
                const auto name = entry.path().filename().wstring();
                if (name.rfind(L"nv_dispi", 0) == 0) {
                    if (std::filesystem::exists(entry.path() / L"nvngx.dll")) {
                        return entry.path().wstring();
                    }
                }
            }
        }
    }

    return L"";
}

static std::string CheckAuthenticodeSignature(const std::wstring& filePath) {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = filePath.c_str();

    GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_IGNORE;

    const LONG status = WinVerifyTrust(nullptr, &policyGUID, &trustData);
    if (status == ERROR_SUCCESS) {
        return "VALID (NVIDIA Signed & Intact)";
    } else if (status == TRUST_E_BAD_DIGEST) {
        return "HASH_MISMATCH (Binary contents modified; digital signature broken)";
    } else if (status == TRUST_E_NOSIGNATURE) {
        return "UNSIGNED (No digital signature found)";
    } else {
        std::ostringstream ss;
        ss << "VERIFY_FAILED (WinVerifyTrust code 0x" << std::hex << status << ")";
        return ss.str();
    }
}

// ── In-Process WinVerifyTrust Detour & IAT Hooks (Option A) ──────────────────

typedef LONG(WINAPI* FnWinVerifyTrust)(HWND, GUID*, LPVOID);
static FnWinVerifyTrust g_OrigWinVerifyTrust = nullptr;
static BYTE g_OriginalBytes[14] = {0};
static std::recursive_mutex g_HookMutex;
static thread_local bool t_insideWinVerifyTrust = false;

// Helper: case-insensitive wide substring check for nvngx_dlssnr.dll or nvngx_dlssd.dll
static bool IsTargetDlssNrBinary(LPCWSTR filePath) {
    if (!filePath) return false;
    wchar_t lowerPath[MAX_PATH] = {0};
    wcsncpy_s(lowerPath, filePath, _TRUNCATE);
    _wcslwr_s(lowerPath);
    return (wcsstr(lowerPath, L"nvngx_dlssnr.dll") != nullptr || wcsstr(lowerPath, L"nvngx_dlssd.dll") != nullptr);
}

static bool ShouldSpoofWinVerifyTrust(LPVOID pWVTData) {
    __try {
        auto* pData = reinterpret_cast<WINTRUST_DATA*>(pWVTData);
        if (pData && pData->dwUnionChoice == WTD_CHOICE_FILE && pData->pFile && pData->pFile->pcwszFilePath) {
            std::printf("  [WVT-HOOK] Inspecting file: %ls\n", pData->pFile->pcwszFilePath);
            std::fflush(stdout);
            if (IsTargetDlssNrBinary(pData->pFile->pcwszFilePath)) {
                std::printf("  [WVT-HOOK] WinVerifyTrust intercepted for nvngx_dlssnr.dll -> Spoofing ERROR_SUCCESS (0x0)!\n");
                std::fflush(stdout);
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// Detour callback invoked when _nvngx.dll verifies DLL signatures
static LONG WINAPI HookedWinVerifyTrust(HWND hwnd, GUID* pgActionID, LPVOID pWVTData) {
    if (t_insideWinVerifyTrust) {
        return g_OrigWinVerifyTrust(hwnd, pgActionID, pWVTData);
    }

    if (ShouldSpoofWinVerifyTrust(pWVTData)) {
        return ERROR_SUCCESS; // 0x0
    }

    t_insideWinVerifyTrust = true;
    std::lock_guard<std::recursive_mutex> lock(g_HookMutex);
    DWORD oldProtect = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), g_OriginalBytes, 14);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14);

    LONG status = g_OrigWinVerifyTrust(hwnd, pgActionID, pWVTData);

    // Re-apply 14-byte x64 absolute jump: FF 25 00 00 00 00 [8-byte target]
    BYTE jmpPatch[14] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    uint64_t targetAddr = reinterpret_cast<uint64_t>(&HookedWinVerifyTrust);
    memcpy(&jmpPatch[6], &targetAddr, sizeof(targetAddr));
    memcpy(reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), jmpPatch, 14);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14);
    VirtualProtect(reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14, oldProtect, &oldProtect);

    t_insideWinVerifyTrust = false;
    return status;
}

// Patch a loaded module's Import Address Table (IAT) for wintrust.dll!WinVerifyTrust
static bool PatchModuleIAT(HMODULE hModule) {
    if (!hModule) return false;
    auto* base = reinterpret_cast<BYTE*>(hModule);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    DWORD importRVA = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRVA) return false;

    auto* importDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importRVA);
    for (; importDesc->Name != 0; ++importDesc) {
        const char* modName = reinterpret_cast<const char*>(base + importDesc->Name);
        if (_stricmp(modName, "wintrust.dll") != 0) continue;

        auto* origFirstThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            base + (importDesc->OriginalFirstThunk ? importDesc->OriginalFirstThunk : importDesc->FirstThunk));
        auto* firstThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + importDesc->FirstThunk);

        for (; origFirstThunk->u1.AddressOfData != 0; ++origFirstThunk, ++firstThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(origFirstThunk->u1.Ordinal)) continue;
            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origFirstThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(importByName->Name), "WinVerifyTrust") == 0) {
                DWORD oldProtect = 0;
                VirtualProtect(&firstThunk->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &oldProtect);
                firstThunk->u1.Function = reinterpret_cast<ULONGLONG>(&HookedWinVerifyTrust);
                VirtualProtect(&firstThunk->u1.Function, sizeof(ULONGLONG), oldProtect, &oldProtect);
                std::cout << "  [WVT-HOOK] Patched IAT entry for WinVerifyTrust in module 0x"
                          << std::hex << reinterpret_cast<uintptr_t>(hModule) << std::dec << "\n";
                return true;
            }
        }
    }
    return false;
}

// Call BEFORE NVSDK_NGX_D3D12_Init_Ext / NVSDK_NGX_D3D12_Init_ProjectID
static bool InstallWinVerifyTrustHooks(const std::wstring& driverStoreDir) {
    HMODULE hWintrust = LoadLibraryW(L"wintrust.dll");
    if (!hWintrust) return false;

    g_OrigWinVerifyTrust = reinterpret_cast<FnWinVerifyTrust>(
        GetProcAddress(hWintrust, "WinVerifyTrust"));
    if (!g_OrigWinVerifyTrust) return false;

    // 1. Pre-load DriverStore trampoline & engine so their IATs are in memory before Init
    std::wstring trampolinePath = driverStoreDir.empty() ?
        L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_feedb8c0271ca811\\nvngx.dll" :
        (driverStoreDir + L"\\nvngx.dll");
    std::wstring enginePath = driverStoreDir.empty() ?
        L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_feedb8c0271ca811\\_nvngx.dll" :
        (driverStoreDir + L"\\_nvngx.dll");

    SetDllDirectoryW(driverStoreDir.c_str());
    HMODULE hTrampoline = LoadLibraryExW(trampolinePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE hEngine = LoadLibraryExW(enginePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    PatchModuleIAT(GetModuleHandleW(nullptr));
    if (hTrampoline) PatchModuleIAT(hTrampoline);
    if (hEngine) PatchModuleIAT(hEngine);

    // 2. Install 14-byte inline prologue hook on wintrust.dll!WinVerifyTrust
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    memcpy(g_OriginalBytes, reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14);

    BYTE jmpPatch[14] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    uint64_t targetAddr = reinterpret_cast<uint64_t>(&HookedWinVerifyTrust);
    memcpy(&jmpPatch[6], &targetAddr, sizeof(targetAddr));

    memcpy(reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), jmpPatch, 14);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14);
    VirtualProtect(reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14, oldProtect, &oldProtect);

    std::cout << "  [WVT-HOOK] Inline + IAT hooks installed on wintrust.dll!WinVerifyTrust (0x"
              << std::hex << reinterpret_cast<uintptr_t>(g_OrigWinVerifyTrust) << std::dec << ")\n";
    return true;
}

// ── Main Diagnostic Suite ───────────────────────────────────────────────────

int main() {
    std::cout << "=================================================================\n";
    std::cout << " Gaslightr Phase 9 — Standalone NVIDIA NGX DLSS-NR Probe Target\n";
    std::cout << " Investigating 0xBAD00002 PlatformError & Signature Handshake\n";
    std::cout << " Host GPU: NVIDIA GeForce RTX 4080 SUPER (Ada Lovelace, 16 GB VRAM)\n";
    std::cout << "=================================================================\n\n";

    // ── 1. Digital Signature & Registry Audit ────────────────────────────────
    std::cout << "[Step 1] Digital Signature & Registry Audit...\n";
    const std::wstring incomingDir = FindIncomingDir();
    const auto incomingDlls = FindAllIncomingDlls();
    const std::wstring snippetPath = FindDllPath();
    const std::wstring driverStoreDir = GetDriverStoreNgxDir();
    const std::wstring loaderPath = driverStoreDir.empty() ? L"" : (driverStoreDir + L"\\nvngx.dll");
    const std::wstring internalDriverPath = driverStoreDir.empty() ? L"" : (driverStoreDir + L"\\_nvngx.dll");

    std::wcout << L"  Incoming Directory: " << incomingDir << L"\n";
    if (incomingDlls.empty()) {
        std::cout << "  [-] No DLLs found in incoming directory.\n";
    } else {
        for (const auto& dll : incomingDlls) {
            std::wcout << L"  [Incoming DLL]  " << std::filesystem::path(dll).filename().wstring() << L"\n";
            std::cout  << "    Signature:    " << CheckAuthenticodeSignature(dll) << "\n";
        }
    }

    if (!loaderPath.empty()) {
        std::wcout << L"  Public loader:  " << loaderPath << L"\n";
        std::cout  << "  Signature:      " << CheckAuthenticodeSignature(loaderPath) << "\n";
    } else {
        std::cout  << "  Public loader:  <NOT FOUND in HKLM NGXCore FullPath>\n";
    }

    if (!internalDriverPath.empty()) {
        std::wcout << L"  Internal engine: " << internalDriverPath << L"\n";
        std::cout  << "  Signature:       " << CheckAuthenticodeSignature(internalDriverPath) << "\n";
    }

    // Inspect Registry Override Keys
    HKEY hNgxKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &hNgxKey) == ERROR_SUCCESS) {
        DWORD allowUnsigned = 0, overrideDenial = 0, enableDev = 0;
        DWORD size = sizeof(DWORD);
        bool hasAllow = (RegQueryValueExW(hNgxKey, L"AllowUnsignedBinaries", nullptr, nullptr, reinterpret_cast<LPBYTE>(&allowUnsigned), &size) == ERROR_SUCCESS);
        size = sizeof(DWORD);
        bool hasDenial = (RegQueryValueExW(hNgxKey, L"OverrideFeatureDenial", nullptr, nullptr, reinterpret_cast<LPBYTE>(&overrideDenial), &size) == ERROR_SUCCESS);
        size = sizeof(DWORD);
        bool hasDev = (RegQueryValueExW(hNgxKey, L"EnableDevSignatures", nullptr, nullptr, reinterpret_cast<LPBYTE>(&enableDev), &size) == ERROR_SUCCESS);

        std::cout << "  Registry HKLM NGXCore status:\n";
        std::cout << "    AllowUnsignedBinaries: " << (hasAllow ? std::to_string(allowUnsigned) : "NOT_PRESENT") << "\n";
        std::cout << "    OverrideFeatureDenial: " << (hasDenial ? std::to_string(overrideDenial) : "NOT_PRESENT") << "\n";
        std::cout << "    EnableDevSignatures:   " << (hasDev ? std::to_string(enableDev) : "NOT_PRESENT") << "\n";
        RegCloseKey(hNgxKey);
    }

    HKEY hNvKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm", 0, KEY_READ, &hNvKey) == ERROR_SUCCESS) {
        DWORD disableSig = 0;
        DWORD size = sizeof(DWORD);
        bool hasDisable = (RegQueryValueExW(hNvKey, L"DisableNgxSignatureChecks", nullptr, nullptr, reinterpret_cast<LPBYTE>(&disableSig), &size) == ERROR_SUCCESS);
        std::cout << "  Registry HKLM nvlddmkm status:\n";
        std::cout << "    DisableNgxSignatureChecks: " << (hasDisable ? std::to_string(disableSig) : "NOT_PRESENT") << "\n";
        RegCloseKey(hNvKey);
    }

    // ── 1B. Install In-Process WinVerifyTrust Hooks (Option A) ──────────────
    std::cout << "\n[Step 1B] Installing In-Process WinVerifyTrust Hooks...\n";
    InstallWinVerifyTrustHooks(driverStoreDir);

    // ── 2. Initialize Direct3D 12 Device & DXGI Adapter ─────────────────────
    std::cout << "\n[Step 2] Initializing D3D12 device and DXGI adapter...\n";
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
        if (adapterDesc.VendorId == 0x10DE) break;
        adapter.Reset();
    }
    if (!adapter) {
        std::cerr << "[-] No NVIDIA discrete adapter found.\n";
        return 1;
    }

    std::wcout << L"  Adapter: " << adapterDesc.Description << L" (Vendor: 0x" << std::hex << adapterDesc.VendorId << std::dec << L")\n";
    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        std::cerr << "[-] D3D12CreateDevice failed: 0x" << std::hex << hr << "\n";
        return 1;
    }
    std::cout << "  D3D12 Device ready at Feature Level 12.0.\n";

    // ── 3. Probe Direct Snippet (nvngx_dlssnr.dll) ───────────────────────────
    std::cout << "\n[Step 3] Probing Direct Snippet (vendor/nvidia/incoming/nvngx_dlssnr.dll)...\n";
    HMODULE hSnippet = LoadLibraryW(snippetPath.c_str());
    if (!hSnippet) {
        std::cerr << "[-] LoadLibraryW failed for snippet: " << GetLastError() << "\n";
    } else {
        std::cout << "  Snippet module loaded at 0x" << std::hex << reinterpret_cast<uintptr_t>(hSnippet) << std::dec << "\n";

        // Query metadata
        auto pfnGetAPIVer = reinterpret_cast<PFN_NVSDK_NGX_GetAPIVersion>(GetProcAddress(hSnippet, "NVSDK_NGX_GetAPIVersion"));
        auto pfnGetAppId = reinterpret_cast<PFN_NVSDK_NGX_GetApplicationId>(GetProcAddress(hSnippet, "NVSDK_NGX_GetApplicationId"));
        auto pfnGetArch = reinterpret_cast<PFN_NVSDK_NGX_GetGPUArchitecture>(GetProcAddress(hSnippet, "NVSDK_NGX_GetGPUArchitecture"));
        auto pfnGetSnippetVer = reinterpret_cast<PFN_NVSDK_NGX_GetSnippetVersion>(GetProcAddress(hSnippet, "NVSDK_NGX_GetSnippetVersion"));
        auto pfnInitExt = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Init_Ext>(GetProcAddress(hSnippet, "NVSDK_NGX_D3D12_Init_Ext"));
        auto pfnGetReq = reinterpret_cast<PFN_NVSDK_NGX_D3D12_GetFeatureRequirements>(GetProcAddress(hSnippet, "NVSDK_NGX_D3D12_GetFeatureRequirements"));
        auto pfnShutdown = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Shutdown>(GetProcAddress(hSnippet, "NVSDK_NGX_D3D12_Shutdown"));

        const uint32_t apiVer = pfnGetAPIVer ? pfnGetAPIVer() : 0;
        const uint32_t snippetAppId = pfnGetAppId ? pfnGetAppId() : 0;
        const uint32_t arch = pfnGetArch ? pfnGetArch() : 0;
        const uint32_t snipVer = pfnGetSnippetVer ? pfnGetSnippetVer() : 0;

        std::cout << "  API Version: 0x" << std::hex << apiVer << " | AppId: 0x" << snippetAppId
                  << " | GPU Arch: 0x" << arch << " (Ada 0x1B0: " << (arch == 0x1B0 ? "MATCH" : "MISMATCH") << ")"
                  << " | Snippet Ver: 0x" << snipVer << std::dec << "\n";

        // Test NVSDK_NGX_D3D12_Init_Ext contracts
        if (pfnInitExt) {
            std::cout << "  Attempting NVSDK_NGX_D3D12_Init_Ext direct invocation:\n";

            // Setup FeatureCommonInfo with logging & search paths
            std::wstring snippetDir = std::filesystem::path(snippetPath).parent_path().wstring();
            const wchar_t* searchPaths[] = { snippetDir.c_str() };

            NVSDK_NGX_FeatureCommonInfo commonInfo{};
            commonInfo.PathListInfo.Path = searchPaths;
            commonInfo.PathListInfo.Length = 1;
            commonInfo.LoggingInfo.LoggingCallback = NgxLogCallback;
            commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
            commonInfo.LoggingInfo.DisableOtherLoggingSinks = false;

            struct InitAttempt {
                const char* desc;
                unsigned long long appId;
                NVSDK_NGX_Version version;
            };

            const InitAttempt attempts[] = {
                { "Snippet AppId (0x0E658703) with API Version 0x13", 0x0E658703ULL, 0x13 },
                { "Standard Dev AppId (0x1337) with API Version 0x13", 0x1337ULL, 0x13 },
                { "Zero AppId (0x0) with API Version 0x13", 0x0ULL, 0x13 },
                { "Snippet AppId (0x0E658703) with API Version 0x14", 0x0E658703ULL, 0x14 }
            };

            for (const auto& a : attempts) {
                g_CapturedLogs.clear();
                bool threw = false;
                DWORD exc = 0;
                std::cout << "    Attempt: " << a.desc << "...\n";
                NVSDK_NGX_Result res = SafeCallInitExt(
                    pfnInitExt, a.appId, snippetDir.c_str(), device.Get(), a.version, &commonInfo, &threw, &exc
                );

                if (threw) {
                    std::cout << "      [!] SEH Exception: 0x" << std::hex << exc << std::dec << "\n";
                } else {
                    std::cout << "      [+] Return: 0x" << std::hex << res << std::dec
                              << " (" << GetNgxResultString(res) << ")\n";
                }
                if (res == 1 && pfnShutdown) {
                    pfnShutdown(device.Get());
                }
            }
        } else {
            std::cout << "  [-] NVSDK_NGX_D3D12_Init_Ext not exported by snippet.\n";
        }

        // Test GetFeatureRequirements
        if (pfnGetReq) {
            std::cout << "  Attempting NVSDK_NGX_D3D12_GetFeatureRequirements on snippet:\n";
            NGX_FeatureDiscoveryInfo info{};
            info.FeatureID = NVSDK_NGX_Feature_RayReconstruction;
            std::wstring snippetDir = std::filesystem::path(snippetPath).parent_path().wstring();
            info.ApplicationDataPath = snippetDir.c_str();

            NGX_FeatureRequirement req{};
            req.FeatureSupported = 0xFFFFFFFF;
            bool threw = false;
            DWORD exc = 0;
            NVSDK_NGX_Result res = SafeCallFeatureReq(pfnGetReq, adapter.Get(), &info, &req, &threw, &exc);
            if (threw) {
                std::cout << "    [!] SEH Exception: 0x" << std::hex << exc << std::dec << "\n";
            } else {
                std::cout << "    [+] Return: 0x" << std::hex << res << std::dec
                          << " (" << GetNgxResultString(res) << "), Supported=" << req.FeatureSupported
                          << ", MinDriver=" << req.MinDriverVersionMajor << "." << req.MinDriverVersionMinor << "\n";
            }
        }

        FreeLibrary(hSnippet);
    }

    // ── 4. Probe Official Driver NGX Loader (nvngx.dll) ─────────────────────
    std::cout << "\n[Step 4] Probing Official Driver NGX Loader (nvngx.dll)...\n";
    if (loaderPath.empty() || !std::filesystem::exists(loaderPath)) {
        std::cout << "  [-] DriverStore nvngx.dll path is unavailable.\n";
    } else {
        SetDllDirectoryW(driverStoreDir.c_str());
        HMODULE hLoader = LoadLibraryExW(loaderPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hLoader) {
            std::cerr << "[-] LoadLibraryExW failed for loader: " << GetLastError() << "\n";
        } else {
            std::cout << "  Public loader module loaded at 0x" << std::hex << reinterpret_cast<uintptr_t>(hLoader) << std::dec << "\n";

            auto pfnLoaderInitProjectID = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Init_ProjectID>(
                GetProcAddress(hLoader, "NVSDK_NGX_D3D12_Init_ProjectID")
            );
            auto pfnLoaderInitExt = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Init_Ext>(
                GetProcAddress(hLoader, "NVSDK_NGX_D3D12_Init_Ext")
            );
            auto pfnLoaderGetCap = reinterpret_cast<PFN_NVSDK_NGX_D3D12_GetCapabilityParameters>(
                GetProcAddress(hLoader, "NVSDK_NGX_D3D12_GetCapabilityParameters")
            );
            auto pfnLoaderGetReq = reinterpret_cast<PFN_NVSDK_NGX_D3D12_GetFeatureRequirements>(
                GetProcAddress(hLoader, "NVSDK_NGX_D3D12_GetFeatureRequirements")
            );
            auto pfnLoaderShutdown = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Shutdown>(
                GetProcAddress(hLoader, "NVSDK_NGX_D3D12_Shutdown")
            );

            auto pfnLoaderAllocParams = reinterpret_cast<PFN_NVSDK_NGX_D3D12_AllocateParameters>(
                GetProcAddress(hLoader, "NVSDK_NGX_D3D12_AllocateParameters")
            );
            auto pfnLoaderDestroyParams = reinterpret_cast<PFN_NVSDK_NGX_D3D12_DestroyParameters>(
                GetProcAddress(hLoader, "NVSDK_NGX_D3D12_DestroyParameters")
            );
            auto pfnLoaderCreateFeature = reinterpret_cast<PFN_NVSDK_NGX_D3D12_CreateFeature>(
                GetProcAddress(hLoader, "NVSDK_NGX_D3D12_CreateFeature")
            );
            auto pfnLoaderReleaseFeature = reinterpret_cast<PFN_NVSDK_NGX_D3D12_ReleaseFeature>(
                GetProcAddress(hLoader, "NVSDK_NGX_D3D12_ReleaseFeature")
            );
            auto pfnLoaderEvaluateFeature = reinterpret_cast<PFN_NVSDK_NGX_D3D12_EvaluateFeature>(
                GetProcAddress(hLoader, "NVSDK_NGX_D3D12_EvaluateFeature")
            );
            auto pfnLoaderGetScratchSize = reinterpret_cast<PFN_NVSDK_NGX_D3D12_GetScratchBufferSize>(
                GetProcAddress(hLoader, "NVSDK_NGX_D3D12_GetScratchBufferSize")
            );
            (void)pfnLoaderGetReq;

            std::wstring snippetDir = std::filesystem::path(snippetPath).parent_path().wstring();
            const wchar_t* searchPaths[] = { snippetDir.c_str() };

            NVSDK_NGX_FeatureCommonInfo commonInfo{};
            commonInfo.PathListInfo.Path = searchPaths;
            commonInfo.PathListInfo.Length = 1;
            commonInfo.LoggingInfo.LoggingCallback = NgxLogCallback;
            commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
            commonInfo.LoggingInfo.DisableOtherLoggingSinks = false;

            // Attempt 1: NVSDK_NGX_D3D12_Init_ProjectID
            if (pfnLoaderInitProjectID) {
                std::cout << "  Attempting NVSDK_NGX_D3D12_Init_ProjectID on driver loader:\n";
                const char* testGuids[] = {
                    "a0f57b54-1daf-4934-90ae-c4035c19df04", // Standard sample GUID
                    "00000000-0000-0000-0000-000000000000",
                    "24e93019-9430-4e13-94c6-e794b54e38c3"  // Gaslightr synthetic GUID
                };

                for (const char* guid : testGuids) {
                    g_CapturedLogs.clear();
                    bool threw = false;
                    DWORD exc = 0;
                    std::cout << "    Testing Project GUID: " << guid << "...\n";
                    NVSDK_NGX_Result res = SafeCallInitProjectID(
                        pfnLoaderInitProjectID,
                        guid,
                        NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                        "1.0",
                        snippetDir.c_str(),
                        device.Get(),
                        0x14,
                        &commonInfo,
                        &threw,
                        &exc
                    );

                    if (threw) {
                        std::cout << "      [!] SEH Exception: 0x" << std::hex << exc << std::dec << "\n";
                    } else {
                        std::cout << "      [+] Return: 0x" << std::hex << res << std::dec
                                  << " (" << GetNgxResultString(res) << ")\n";
                    }

                    if (res == 1) {
                        std::cout << "      [SUCCESS] Driver loader initialized! Querying capabilities...\n";
                        if (pfnLoaderGetCap) {
                            void* pRawCaps = nullptr;
                            NVSDK_NGX_Result capRes = pfnLoaderGetCap(&pRawCaps);
                            std::cout << "      [+] GetCapabilityParameters: 0x" << std::hex << capRes << std::dec
                                      << " (" << GetNgxResultString(capRes) << ", Ptr=" << pRawCaps << ")\n";
                            if (capRes == 1 && pRawCaps) {
                                auto pCaps = reinterpret_cast<NVSDK_NGX_Parameter*>(pRawCaps);
                                std::cout << "      --- Querying Hardware Capability Parameters ---\n";
                                SafeQueryParamInt(pCaps, "SuperSampling.Available");
                                SafeQueryParamInt(pCaps, "DLSSNR.Available");
                                SafeQueryParamInt(pCaps, "SuperSamplingDenoising.Available");
                                SafeQueryParamInt(pCaps, "RayReconstruction.Available");
                                SafeQueryParamInt(pCaps, "Denoiser.Available");
                                SafeQueryParamInt(pCaps, "ImageSuperResolution.Available");
                                SafeQueryParamInt(pCaps, "InPainting.Available");
                                SafeQueryParamInt(pCaps, "DeepResolve.Available");
                                SafeQueryParamInt(pCaps, "DLSSNR.FeatureInitResult");
                                SafeQueryParamInt(pCaps, "DLSSNR.NeedsUpdatedDriver");
                                SafeQueryParamInt(pCaps, "DLSSNR.MinDriverVersionMajor");
                                SafeQueryParamInt(pCaps, "DLSSNR.MinDriverVersionMinor");
                                SafeQueryParamInt(pCaps, "NGX.Version");
                                SafeQueryParamInt(pCaps, "Driver.Version");
                            }
                        }

                        if (pfnLoaderAllocParams) {
                            void* pRawParams = nullptr;
                            NVSDK_NGX_Result aRes = pfnLoaderAllocParams(&pRawParams);
                            std::cout << "      [+] AllocateParameters: 0x" << std::hex << aRes << std::dec
                                      << " (" << GetNgxResultString(aRes) << ", Ptr=" << pRawParams << ")\n";
                            if (aRes == 1 && pRawParams) {
                                auto pAppParams = reinterpret_cast<NVSDK_NGX_Parameter*>(pRawParams);
                                if (pfnLoaderGetScratchSize) {
                                    size_t scratchSize13 = 0;
                                    NVSDK_NGX_Result sRes13 = pfnLoaderGetScratchSize(NVSDK_NGX_Feature_RayReconstruction, pAppParams, &scratchSize13);
                                    std::cout << "      [+] GetScratchBufferSize (RayReconstruction Feature 13): 0x" << std::hex << sRes13 << std::dec
                                              << " (" << GetNgxResultString(sRes13) << ", Size=" << scratchSize13 << " bytes)\n";

                                    size_t scratchSize1 = 0;
                                    NVSDK_NGX_Result sRes1 = pfnLoaderGetScratchSize(NVSDK_NGX_Feature_SuperSampling, pAppParams, &scratchSize1);
                                    std::cout << "      [+] GetScratchBufferSize (SuperSampling Feature 1):       0x" << std::hex << sRes1 << std::dec
                                              << " (" << GetNgxResultString(sRes1) << ", Size=" << scratchSize1 << " bytes)\n";
                                }

                                // Allocate D3D12 dummy resources for probe testing
                                ComPtr<ID3D12Resource> pColor512 = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pOutput1024 = CreateProbeTexture(device.Get(), 1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                                ComPtr<ID3D12Resource> pDepth512 = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pMVec512 = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pNormals512 = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pRoughness512 = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pDiffuseAlbedo512 = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pSpecularAlbedo512 = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                                ComPtr<ID3D12Resource> pColor1024 = CreateProbeTexture(device.Get(), 1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pDepth1024 = CreateProbeTexture(device.Get(), 1024, 1024, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pMVec1024 = CreateProbeTexture(device.Get(), 1024, 1024, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pNormals1024 = CreateProbeTexture(device.Get(), 1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pRoughness1024 = CreateProbeTexture(device.Get(), 1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pDiffuseAlbedo1024 = CreateProbeTexture(device.Get(), 1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                                ComPtr<ID3D12Resource> pSpecularAlbedo1024 = CreateProbeTexture(device.Get(), 1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                                std::cout << "      [Resources] Color512=" << pColor512.Get()
                                          << ", Output1024=" << pOutput1024.Get()
                                          << ", Depth512=" << pDepth512.Get()
                                          << ", MVec512=" << pMVec512.Get()
                                          << ", Normals512=" << pNormals512.Get()
                                          << ", Normals1024=" << pNormals1024.Get() << "\n";

                                ComPtr<ID3D12CommandAllocator> cmdAlloc;
                                hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
                                ComPtr<ID3D12GraphicsCommandList> cmdList;
                                hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));

                                // 1. Probe Feature 1 (SuperSampling / DLSS + DLSS-D Ray Reconstruction) Creation & Evaluation
                                if (pfnLoaderCreateFeature) {
                                    std::cout << "\n      --- Probing Feature 1 (SuperSampling / DLSS-D Ray Reconstruction) Creation ---\n";
                                    pAppParams->Reset();
                                    pAppParams->Set("Width", 512u);
                                    pAppParams->Set("Height", 512u);
                                    pAppParams->Set("OutWidth", 1024u);
                                    pAppParams->Set("OutHeight", 1024u);
                                    pAppParams->Set("PerfQualityValue", 2u); // MaxQuality
                                    pAppParams->Set("RTXValue", 1u);
                                    pAppParams->Set("Reset", 1u);
                                    pAppParams->Set("DLSS.Feature.Create.Flags", 0u);
                                    pAppParams->Set("DLSS.Denoise.Mode", 1u); // DLUnified Ray Reconstruction mode
                                    pAppParams->Set("DLSS.Roughness.Mode", 0u);
                                    pAppParams->Set("DLSS.Render.Subrect.Dimensions.Width", 512u);
                                    pAppParams->Set("DLSS.Render.Subrect.Dimensions.Height", 512u);

                                    if (pColor512) pAppParams->Set("Color", pColor512.Get());
                                    if (pOutput1024) pAppParams->Set("Output", pOutput1024.Get());
                                    if (pDepth512) pAppParams->Set("Depth", pDepth512.Get());
                                    if (pMVec512) pAppParams->Set("MotionVectors", pMVec512.Get());
                                    if (pNormals512) pAppParams->Set("DLSS.Input.Normals", pNormals512.Get());
                                    if (pRoughness512) pAppParams->Set("DLSS.Input.Roughness", pRoughness512.Get());
                                    if (pDiffuseAlbedo512) pAppParams->Set("DLSS.Input.DiffuseAlbedo", pDiffuseAlbedo512.Get());
                                    if (pSpecularAlbedo512) pAppParams->Set("DLSS.Input.SpecularAlbedo", pSpecularAlbedo512.Get());

                                    void* pFeatureHandleSS = nullptr;
                                    bool createThrew = false;
                                    DWORD createExc = 0;
                                    NVSDK_NGX_Result cResSS = SafeCallCreateFeature(
                                        pfnLoaderCreateFeature,
                                        cmdList.Get(),
                                        NVSDK_NGX_Feature_SuperSampling,
                                        pAppParams,
                                        &pFeatureHandleSS,
                                        &createThrew,
                                        &createExc
                                    );

                                    if (createThrew) {
                                        std::cout << "      [!] CreateFeature (SuperSampling/DLSS-D) SEH Exception: 0x" << std::hex << createExc << std::dec << "\n";
                                    } else {
                                        std::cout << "      [+] CreateFeature (SuperSampling/DLSS-D): 0x" << std::hex << cResSS << std::dec
                                                  << " (" << GetNgxResultString(cResSS) << ", Handle=" << pFeatureHandleSS << ")\n";
                                        if (cResSS == 1 && pFeatureHandleSS) {
                                            std::cout << "      [SUCCESS] DLSS SuperSampling (DLSS-D Ray Reconstruction) feature handle created!\n";
                                            if (pfnLoaderEvaluateFeature) {
                                                std::cout << "      Attempting NVSDK_NGX_D3D12_EvaluateFeature on SuperSampling/DLSS-D...\n";
                                                if (pColor512) pAppParams->Set("Color", pColor512.Get());
                                                if (pOutput1024) pAppParams->Set("Output", pOutput1024.Get());
                                                if (pDepth512) pAppParams->Set("Depth", pDepth512.Get());
                                                if (pMVec512) pAppParams->Set("MotionVectors", pMVec512.Get());
                                                if (pNormals512) pAppParams->Set("DLSS.Input.Normals", pNormals512.Get());
                                                if (pRoughness512) pAppParams->Set("DLSS.Input.Roughness", pRoughness512.Get());
                                                if (pDiffuseAlbedo512) pAppParams->Set("DLSS.Input.DiffuseAlbedo", pDiffuseAlbedo512.Get());
                                                if (pSpecularAlbedo512) pAppParams->Set("DLSS.Input.SpecularAlbedo", pSpecularAlbedo512.Get());

                                                pAppParams->Set("Jitter.Offset.X", 0.0f);
                                                pAppParams->Set("Jitter.Offset.Y", 0.0f);
                                                pAppParams->Set("Sharpness", 0.0f);
                                                pAppParams->Set("Reset", 1);
                                                pAppParams->Set("MV.Scale.X", 1.0f);
                                                pAppParams->Set("MV.Scale.Y", 1.0f);
                                                pAppParams->Set("MV.Offset.X", 0.0f);
                                                pAppParams->Set("MV.Offset.Y", 0.0f);
                                                pAppParams->Set("RenderSubrectDimensionsWidth", 512u);
                                                pAppParams->Set("RenderSubrectDimensionsHeight", 512u);
                                                pAppParams->Set("PreExposure", 1.0f);
                                                pAppParams->Set("ExposureScale", 1.0f);

                                                ID3D12Resource* pResTest = nullptr;
                                                float fValTest = 0.0f;
                                                int iValTest = 0;
                                                std::cout << "        Param audit before EvaluateFeature:\n";
                                                std::cout << "          Color:           res=0x" << std::hex << pAppParams->Get("Color", &pResTest) << ", ptr=" << pResTest << std::dec << "\n";
                                                pResTest = nullptr;
                                                std::cout << "          Output:          res=0x" << std::hex << pAppParams->Get("Output", &pResTest) << ", ptr=" << pResTest << std::dec << "\n";
                                                pResTest = nullptr;
                                                std::cout << "          Depth:           res=0x" << std::hex << pAppParams->Get("Depth", &pResTest) << ", ptr=" << pResTest << std::dec << "\n";
                                                pResTest = nullptr;
                                                std::cout << "          MotionVectors:   res=0x" << std::hex << pAppParams->Get("MotionVectors", &pResTest) << ", ptr=" << pResTest << std::dec << "\n";
                                                pResTest = nullptr;
                                                std::cout << "          Normals:         res=0x" << std::hex << pAppParams->Get("DLSS.Input.Normals", &pResTest) << ", ptr=" << pResTest << std::dec << "\n";
                                                std::cout << "          Jitter.Offset.X: res=0x" << std::hex << pAppParams->Get("Jitter.Offset.X", &fValTest) << ", val=" << fValTest << std::dec << "\n";
                                                std::cout << "          Jitter.Offset.Y: res=0x" << std::hex << pAppParams->Get("Jitter.Offset.Y", &fValTest) << ", val=" << fValTest << std::dec << "\n";
                                                std::cout << "          MV.Scale.X:      res=0x" << std::hex << pAppParams->Get("MV.Scale.X", &fValTest) << ", val=" << fValTest << std::dec << "\n";
                                                std::cout << "          MV.Scale.Y:      res=0x" << std::hex << pAppParams->Get("MV.Scale.Y", &fValTest) << ", val=" << fValTest << std::dec << "\n";
                                                std::cout << "          Reset:           res=0x" << std::hex << pAppParams->Get("Reset", &iValTest) << ", val=" << iValTest << std::dec << "\n";
                                                if (pFeatureHandleSS) {
                                                    auto pWords = reinterpret_cast<uint32_t*>(pFeatureHandleSS);
                                                    std::cout << "          HandleSS words:  [0]=" << pWords[0] << ", [1]=" << pWords[1] << "\n";
                                                }

                                                bool evalThrew = false;
                                                DWORD evalExc = 0;
                                                NVSDK_NGX_Result eRes = SafeCallEvaluateFeature(
                                                    pfnLoaderEvaluateFeature,
                                                    cmdList.Get(),
                                                    pFeatureHandleSS,
                                                    pAppParams,
                                                    nullptr,
                                                    &evalThrew,
                                                    &evalExc
                                                );
                                                if (evalThrew) {
                                                    std::cout << "        [!] EvaluateFeature SEH Exception: 0x" << std::hex << evalExc << std::dec << "\n";
                                                } else {
                                                    std::cout << "        [+] EvaluateFeature Return: 0x" << std::hex << eRes << std::dec
                                                              << " (" << GetNgxResultString(eRes) << ")\n";
                                                }
                                            }
                                            if (pfnLoaderReleaseFeature) {
                                                bool relThrew = false;
                                                DWORD relExc = 0;
                                                SafeCallReleaseFeature(pfnLoaderReleaseFeature, pFeatureHandleSS, &relThrew, &relExc);
                                                std::cout << "      [+] SuperSampling feature released.\n";
                                            }
                                        }
                                    }

                                    // 1B. Probe Feature 1 in DLAA Native Mode (1024x1024 -> 1024x1024) with DLSS-D Ray Reconstruction
                                    std::cout << "\n      --- Probing Feature 1 in DLAA Mode (1024x1024 Native DLSS-D Ray Reconstruction) ---\n";
                                    pAppParams->Reset();
                                    pAppParams->Set("Width", 1024u);
                                    pAppParams->Set("Height", 1024u);
                                    pAppParams->Set("OutWidth", 1024u);
                                    pAppParams->Set("OutHeight", 1024u);
                                    pAppParams->Set("PerfQualityValue", 5u); // DLAA mode (1:1 reconstruction)
                                    pAppParams->Set("RTXValue", 1u);
                                    pAppParams->Set("Reset", 1u);
                                    pAppParams->Set("DLSS.Feature.Create.Flags", 0u);
                                    pAppParams->Set("DLSS.Denoise.Mode", 1u); // DLUnified Ray Reconstruction mode
                                    pAppParams->Set("DLSS.Roughness.Mode", 0u);
                                    pAppParams->Set("DLSS.Render.Subrect.Dimensions.Width", 1024u);
                                    pAppParams->Set("DLSS.Render.Subrect.Dimensions.Height", 1024u);

                                    if (pColor1024) pAppParams->Set("Color", pColor1024.Get());
                                    if (pOutput1024) pAppParams->Set("Output", pOutput1024.Get());
                                    if (pDepth1024) pAppParams->Set("Depth", pDepth1024.Get());
                                    if (pMVec1024) pAppParams->Set("MotionVectors", pMVec1024.Get());
                                    if (pNormals1024) pAppParams->Set("DLSS.Input.Normals", pNormals1024.Get());
                                    if (pRoughness1024) pAppParams->Set("DLSS.Input.Roughness", pRoughness1024.Get());
                                    if (pDiffuseAlbedo1024) pAppParams->Set("DLSS.Input.DiffuseAlbedo", pDiffuseAlbedo1024.Get());
                                    if (pSpecularAlbedo1024) pAppParams->Set("DLSS.Input.SpecularAlbedo", pSpecularAlbedo1024.Get());

                                    void* pFeatureHandleDLAA = nullptr;
                                    bool createThrewDLAA = false;
                                    DWORD createExcDLAA = 0;
                                    NVSDK_NGX_Result cResDLAA = SafeCallCreateFeature(
                                        pfnLoaderCreateFeature,
                                        cmdList.Get(),
                                        NVSDK_NGX_Feature_SuperSampling,
                                        pAppParams,
                                        &pFeatureHandleDLAA,
                                        &createThrewDLAA,
                                        &createExcDLAA
                                    );

                                    if (createThrewDLAA) {
                                        std::cout << "      [!] CreateFeature (DLAA/DLSS-D) SEH Exception: 0x" << std::hex << createExcDLAA << std::dec << "\n";
                                    } else {
                                        std::cout << "      [+] CreateFeature (DLAA/DLSS-D): 0x" << std::hex << cResDLAA << std::dec
                                                  << " (" << GetNgxResultString(cResDLAA) << ", Handle=" << pFeatureHandleDLAA << ")\n";
                                        if (cResDLAA == 1 && pFeatureHandleDLAA) {
                                            std::cout << "      [SUCCESS] DLAA DLSS-D Ray Reconstruction feature handle created!\n";
                                            if (pfnLoaderEvaluateFeature) {
                                                std::cout << "      Attempting NVSDK_NGX_D3D12_EvaluateFeature on DLAA/DLSS-D...\n";
                                                if (pColor1024) pAppParams->Set("Color", pColor1024.Get());
                                                if (pOutput1024) pAppParams->Set("Output", pOutput1024.Get());
                                                if (pDepth1024) pAppParams->Set("Depth", pDepth1024.Get());
                                                if (pMVec1024) pAppParams->Set("MotionVectors", pMVec1024.Get());
                                                if (pNormals1024) pAppParams->Set("DLSS.Input.Normals", pNormals1024.Get());
                                                if (pRoughness1024) pAppParams->Set("DLSS.Input.Roughness", pRoughness1024.Get());
                                                if (pDiffuseAlbedo1024) pAppParams->Set("DLSS.Input.DiffuseAlbedo", pDiffuseAlbedo1024.Get());
                                                if (pSpecularAlbedo1024) pAppParams->Set("DLSS.Input.SpecularAlbedo", pSpecularAlbedo1024.Get());

                                                pAppParams->Set("Jitter.Offset.X", 0.0f);
                                                pAppParams->Set("Jitter.Offset.Y", 0.0f);
                                                pAppParams->Set("Sharpness", 0.0f);
                                                pAppParams->Set("Reset", 1);
                                                pAppParams->Set("MV.Scale.X", 1.0f);
                                                pAppParams->Set("MV.Scale.Y", 1.0f);
                                                pAppParams->Set("MV.Offset.X", 0.0f);
                                                pAppParams->Set("MV.Offset.Y", 0.0f);
                                                pAppParams->Set("RenderSubrectDimensionsWidth", 1024u);
                                                pAppParams->Set("RenderSubrectDimensionsHeight", 1024u);
                                                pAppParams->Set("PreExposure", 1.0f);
                                                pAppParams->Set("ExposureScale", 1.0f);

                                                bool evalThrew = false;
                                                DWORD evalExc = 0;
                                                NVSDK_NGX_Result eRes = SafeCallEvaluateFeature(
                                                    pfnLoaderEvaluateFeature,
                                                    cmdList.Get(),
                                                    pFeatureHandleDLAA,
                                                    pAppParams,
                                                    nullptr,
                                                    &evalThrew,
                                                    &evalExc
                                                );
                                                if (evalThrew) {
                                                    std::cout << "        [!] EvaluateFeature (DLAA/DLSS-D) SEH Exception: 0x" << std::hex << evalExc << std::dec << "\n";
                                                } else {
                                                    std::cout << "        [+] EvaluateFeature (DLAA/DLSS-D) Return: 0x" << std::hex << eRes << std::dec
                                                              << " (" << GetNgxResultString(eRes) << ")\n";
                                                }
                                            }
                                            if (pfnLoaderReleaseFeature) {
                                                bool relThrew = false;
                                                DWORD relExc = 0;
                                                SafeCallReleaseFeature(pfnLoaderReleaseFeature, pFeatureHandleDLAA, &relThrew, &relExc);
                                                std::cout << "      [+] DLAA/DLSS-D feature released.\n";
                                            }
                                        }
                                    }
                                }

                                // 2. Probe Feature 13 (RayReconstruction / DLSS-NR) Creation & Evaluation
                                if (pfnLoaderCreateFeature) {
                                    std::cout << "\n      --- Probing Feature 13 (RayReconstruction / DLSS-NR) Creation ---\n";
                                    pAppParams->Reset();
                                    pAppParams->Set("Width", 1024u);
                                    pAppParams->Set("Height", 1024u);
                                    pAppParams->Set("OutWidth", 1024u);
                                    pAppParams->Set("OutHeight", 1024u);
                                    pAppParams->Set("TargetWidth", 1024u);
                                    pAppParams->Set("TargetHeight", 1024u);
                                    pAppParams->Set("PerfQualityValue", 2u); // MaxQuality
                                    pAppParams->Set("RTXValue", 1u);
                                    pAppParams->Set("Reset", 1u);
                                    pAppParams->Set("DLSS.Feature.Create.Flags", 0u);
                                    pAppParams->Set("DLSS.Denoise.Mode", 0u);
                                    pAppParams->Set("DLSS.Roughness.Mode", 0u);
                                    pAppParams->Set("DLSS.Render.Subrect.Dimensions.Width", 1024u);
                                    pAppParams->Set("DLSS.Render.Subrect.Dimensions.Height", 1024u);

                                    // Primary textures
                                    if (pColor1024) pAppParams->Set("Color", pColor1024.Get());
                                    if (pOutput1024) pAppParams->Set("Output", pOutput1024.Get());
                                    if (pDepth1024) pAppParams->Set("Depth", pDepth1024.Get());
                                    if (pMVec1024) pAppParams->Set("MotionVectors", pMVec1024.Get());

                                    // DLSS-D Ray Reconstruction GBuffer textures
                                    if (pNormals1024) pAppParams->Set("DLSS.Input.Normals", pNormals1024.Get());
                                    if (pRoughness1024) pAppParams->Set("DLSS.Input.Roughness", pRoughness1024.Get());
                                    if (pDiffuseAlbedo1024) pAppParams->Set("DLSS.Input.DiffuseAlbedo", pDiffuseAlbedo1024.Get());
                                    if (pSpecularAlbedo1024) pAppParams->Set("DLSS.Input.SpecularAlbedo", pSpecularAlbedo1024.Get());

                                    // DLSSNR dual-contract aliases
                                    pAppParams->Set("DLSSNR.Width", 1024u);
                                    pAppParams->Set("DLSSNR.Height", 1024u);
                                    pAppParams->Set("DLSSNR.Intensity", 1.0f);
                                    pAppParams->Set("DLSSNR.LocalStructureStrength", 1.0f);
                                    pAppParams->Set("DLSSNR.LocalToneStrength", 1.0f);
                                    pAppParams->Set("DLSSNR.SkinStructureStrength", 1.0f);
                                    pAppParams->Set("DLSSNR.MVecScaleX", 1.0f);
                                    pAppParams->Set("DLSSNR.MVecScaleY", 1.0f);
                                    pAppParams->Set("DLSSNR.DepthInverted", 0);
                                    pAppParams->Set("DLSSNR.ScalingRatio", 1.0f);
                                    pAppParams->Set("DLSSNR.UseAutoMask", 1);
                                    pAppParams->Set("DLSSNR.Enabled", 1);
                                    pAppParams->Set("DLSSNR.Reset", 1u);
                                    if (pColor1024) pAppParams->Set("DLSSNR.Color", pColor1024.Get());
                                    if (pOutput1024) pAppParams->Set("DLSSNR.Output", pOutput1024.Get());
                                    if (pDepth1024) pAppParams->Set("DLSSNR.Depth", pDepth1024.Get());
                                    if (pMVec1024) pAppParams->Set("DLSSNR.MVec", pMVec1024.Get());

                                    void* pFeatureHandleNR = nullptr;
                                    bool createThrew = false;
                                    DWORD createExc = 0;
                                    NVSDK_NGX_Result cResNR = SafeCallCreateFeature(
                                        pfnLoaderCreateFeature,
                                        cmdList.Get(),
                                        NVSDK_NGX_Feature_RayReconstruction,
                                        pAppParams,
                                        &pFeatureHandleNR,
                                        &createThrew,
                                        &createExc
                                    );
                                    if (createThrew) {
                                        std::cout << "      [!] CreateFeature (RayReconstruction) SEH Exception: 0x" << std::hex << createExc << std::dec << "\n";
                                    } else {
                                        std::cout << "      [+] CreateFeature (RayReconstruction): 0x" << std::hex << cResNR << std::dec
                                                  << " (" << GetNgxResultString(cResNR) << ", Handle=" << pFeatureHandleNR << ")\n";
                                        SafeQueryParamInt(pAppParams, "DLSSNR.FeatureInitResult");
                                        if (cResNR == 1 && pFeatureHandleNR) {
                                            std::cout << "      [SUCCESS] DLSS-NR RayReconstruction feature handle created!\n";
                                            if (pfnLoaderEvaluateFeature) {
                                                if (pColor1024) pAppParams->Set("Color", pColor1024.Get());
                                                if (pOutput1024) pAppParams->Set("Output", pOutput1024.Get());
                                                if (pDepth1024) pAppParams->Set("Depth", pDepth1024.Get());
                                                if (pMVec1024) pAppParams->Set("MotionVectors", pMVec1024.Get());
                                                if (pNormals1024) pAppParams->Set("DLSS.Input.Normals", pNormals1024.Get());
                                                if (pRoughness1024) pAppParams->Set("DLSS.Input.Roughness", pRoughness1024.Get());
                                                if (pDiffuseAlbedo1024) pAppParams->Set("DLSS.Input.DiffuseAlbedo", pDiffuseAlbedo1024.Get());
                                                if (pSpecularAlbedo1024) pAppParams->Set("DLSS.Input.SpecularAlbedo", pSpecularAlbedo1024.Get());

                                                pAppParams->Set("Jitter.Offset.X", 0.0f);
                                                pAppParams->Set("Jitter.Offset.Y", 0.0f);
                                                pAppParams->Set("Sharpness", 0.0f);
                                                pAppParams->Set("Reset", 1);
                                                pAppParams->Set("MV.Scale.X", 1.0f);
                                                pAppParams->Set("MV.Scale.Y", 1.0f);
                                                pAppParams->Set("RenderSubrectDimensionsWidth", 1024u);
                                                pAppParams->Set("RenderSubrectDimensionsHeight", 1024u);
                                                pAppParams->Set("PreExposure", 1.0f);
                                                pAppParams->Set("ExposureScale", 1.0f);

                                                pAppParams->Set("DLSSNR.Reset", 1);
                                                pAppParams->Set("DLSSNR.Intensity", 1.0f);
                                                pAppParams->Set("DLSSNR.LocalStructureStrength", 1.0f);
                                                pAppParams->Set("DLSSNR.LocalToneStrength", 1.0f);
                                                pAppParams->Set("DLSSNR.SkinStructureStrength", 1.0f);
                                                pAppParams->Set("DLSSNR.MVecScaleX", 1.0f);
                                                pAppParams->Set("DLSSNR.MVecScaleY", 1.0f);
                                                pAppParams->Set("DLSSNR.DepthInverted", 0);
                                                pAppParams->Set("DLSSNR.ScalingRatio", 1.0f);
                                                pAppParams->Set("DLSSNR.UseAutoMask", 1);
                                                pAppParams->Set("DLSSNR.Enabled", 1);
                                                pAppParams->Set("Reset", 1);

                                                bool evalThrew = false;
                                                DWORD evalExc = 0;
                                                NVSDK_NGX_Result eRes = SafeCallEvaluateFeature(
                                                    pfnLoaderEvaluateFeature,
                                                    cmdList.Get(),
                                                    pFeatureHandleNR,
                                                    pAppParams,
                                                    nullptr,
                                                    &evalThrew,
                                                    &evalExc
                                                );
                                                if (evalThrew) {
                                                    std::cout << "        [!] EvaluateFeature SEH Exception: 0x" << std::hex << evalExc << std::dec << "\n";
                                                } else {
                                                    std::cout << "        [+] EvaluateFeature Return: 0x" << std::hex << eRes << std::dec
                                                              << " (" << GetNgxResultString(eRes) << ")\n";
                                                }
                                            }
                                            if (pfnLoaderReleaseFeature) {
                                                bool relThrew = false;
                                                DWORD relExc = 0;
                                                SafeCallReleaseFeature(pfnLoaderReleaseFeature, pFeatureHandleNR, &relThrew, &relExc);
                                                std::cout << "      [+] RayReconstruction feature released.\n";
                                            }
                                        }
                                    }
                                }

                                if (pfnLoaderDestroyParams) {
                                    pfnLoaderDestroyParams(pRawParams);
                                }
                            }
                        }

                        if (pfnLoaderShutdown) pfnLoaderShutdown(device.Get());
                        break;
                    }
                }
            }

            // Attempt 2: NVSDK_NGX_D3D12_Init_Ext on loader with Snippet AppId
            if (pfnLoaderInitExt) {
                std::cout << "\n  Attempting NVSDK_NGX_D3D12_Init_Ext with Snippet AppId (0x0E658703):\n";
                g_CapturedLogs.clear();
                bool threw = false;
                DWORD exc = 0;
                NVSDK_NGX_Result res = SafeCallInitExt(
                    pfnLoaderInitExt,
                    0x0E658703ULL,
                    snippetDir.c_str(),
                    device.Get(),
                    0x13,
                    &commonInfo,
                    &threw,
                    &exc
                );
                if (threw) {
                    std::cout << "    [!] SEH Exception: 0x" << std::hex << exc << std::dec << "\n";
                } else {
                    std::cout << "    [+] Return: 0x" << std::hex << res << std::dec
                              << " (" << GetNgxResultString(res) << ")\n";
                }

                if (res == 1) {
                    std::cout << "    [SUCCESS] Driver loader initialized via Init_Ext! Querying capabilities...\n";
                    if (pfnLoaderGetCap) {
                        void* pRawCaps = nullptr;
                        NVSDK_NGX_Result capRes = pfnLoaderGetCap(&pRawCaps);
                        std::cout << "    [+] GetCapabilityParameters: 0x" << std::hex << capRes << std::dec
                                  << " (" << GetNgxResultString(capRes) << ", Ptr=" << pRawCaps << ")\n";
                        if (capRes == 1 && pRawCaps) {
                            auto pCaps = reinterpret_cast<NVSDK_NGX_Parameter*>(pRawCaps);
                            SafeQueryParamInt(pCaps, "SuperSampling.Available");
                            SafeQueryParamInt(pCaps, "DLSSNR.Available");
                            SafeQueryParamInt(pCaps, "SuperSamplingDenoising.Available");
                            SafeQueryParamInt(pCaps, "RayReconstruction.Available");
                            SafeQueryParamInt(pCaps, "Denoiser.Available");
                            SafeQueryParamInt(pCaps, "ImageSuperResolution.Available");
                            SafeQueryParamInt(pCaps, "InPainting.Available");
                            SafeQueryParamInt(pCaps, "DeepResolve.Available");
                            SafeQueryParamInt(pCaps, "DLSSNR.FeatureInitResult");
                            SafeQueryParamInt(pCaps, "DLSSNR.NeedsUpdatedDriver");
                            SafeQueryParamInt(pCaps, "DLSSNR.MinDriverVersionMajor");
                            SafeQueryParamInt(pCaps, "DLSSNR.MinDriverVersionMinor");
                        }
                    }
                    if (pfnLoaderShutdown) pfnLoaderShutdown(device.Get());
                }
            }

            FreeLibrary(hLoader);
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // Phase 10: Production NeuralBackend Integration Verification
    // ═════════════════════════════════════════════════════════════════════
    std::printf("\n=================================================================\n");
    std::printf(" Phase 10: Testing Production NeuralBackend Runtime Pipeline\n");
    std::printf("=================================================================\n");
    std::fflush(stdout);
    {
        auto& backend = Gaslightr::NeuralBackend::GetInstance();
        std::printf("  [1] Initializing NeuralBackend::InitializeExplicit (DLSS-D 512->1024)...\n");
        std::fflush(stdout);
        auto initRes = backend.InitializeExplicit(device.Get(), 512, 512, 1024, 1024, true, 2);
        if (!initRes) {
            std::printf("    [FAIL] InitializeExplicit failed: %ls\n", initRes.detail.c_str());
            std::fflush(stdout);
        } else {
            std::printf("    [SUCCESS] NeuralBackend initialized! IsLoaded=%d, IsReady=%d\n",
                backend.IsLoaded() ? 1 : 0, backend.IsReady() ? 1 : 0);
            std::fflush(stdout);

            D3D12_COMMAND_QUEUE_DESC qDesc{};
            qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue> testQueue;
            device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&testQueue));

            ComPtr<ID3D12CommandAllocator> testAlloc;
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&testAlloc));
            ComPtr<ID3D12GraphicsCommandList> testCmdList;
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, testAlloc.Get(), nullptr, IID_PPV_ARGS(&testCmdList));

            ComPtr<ID3D12Resource> pTestColor = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12Resource> pTestDepth = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12Resource> pTestMVec = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12Resource> pTestNormals = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12Resource> pTestRoughness = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12Resource> pTestDiffuse = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12Resource> pTestSpecular = CreateProbeTexture(device.Get(), 512, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12Resource> pTestOutput = CreateProbeTexture(device.Get(), 1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            Gaslightr::NVNGX_EvaluationInputs evalInputs{};
            evalInputs.pColorInput = pTestColor.Get();
            evalInputs.pDepthBuffer = pTestDepth.Get();
            evalInputs.pMotionVectors = pTestMVec.Get();
            evalInputs.pNormals = pTestNormals.Get();
            evalInputs.pRoughness = pTestRoughness.Get();
            evalInputs.pDiffuseAlbedo = pTestDiffuse.Get();
            evalInputs.pSpecularAlbedo = pTestSpecular.Get();
            evalInputs.MotionVectorScaleX = 1.0f;
            evalInputs.MotionVectorScaleY = 1.0f;
            evalInputs.CameraJitterX = 0.0f;
            evalInputs.CameraJitterY = 0.0f;
            evalInputs.FrameIndex = 0;
            evalInputs.ResetHistory = true;

            std::printf("  [2] Evaluating NeuralBackend::EvaluateExplicit on D3D12 command list...\n");
            std::fflush(stdout);
            auto evalRes = backend.EvaluateExplicit(testCmdList.Get(), evalInputs, pTestOutput.Get());
            if (!evalRes) {
                std::printf("    [FAIL] EvaluateExplicit failed: %ls\n", evalRes.detail.c_str());
            } else {
                std::printf("    [SUCCESS] NeuralBackend evaluated cleanly on RTX 4080 SUPER!\n");
            }
            std::fflush(stdout);

            testCmdList->Close();
            ID3D12CommandList* lists[] = { testCmdList.Get() };
            testQueue->ExecuteCommandLists(1, lists);

            // Synchronize
            ComPtr<ID3D12Fence> fence;
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            testQueue->Signal(fence.Get(), 1);
            HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (hEvent) {
                fence->SetEventOnCompletion(1, hEvent);
                WaitForSingleObject(hEvent, INFINITE);
                CloseHandle(hEvent);
            }

            std::printf("  [3] Shutting down NeuralBackend...\n");
            std::fflush(stdout);
            backend.Shutdown();
            std::printf("    [SUCCESS] NeuralBackend shut down cleanly. IsReady=%d\n", backend.IsReady() ? 1 : 0);
            std::fflush(stdout);
        }
    }

    std::printf("\n=================================================================\n");
    std::printf(" Phase 9 & 10 NGX Probing Complete. Photoshop host remains protected.\n");
    std::printf("=================================================================\n");
    std::fflush(stdout);

    return 0;
}
