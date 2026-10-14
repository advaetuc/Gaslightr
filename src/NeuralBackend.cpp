#include "NeuralBackend.h"
#include "BackendCapabilities.h"
#include <wintrust.h>
#include <softpub.h>
#include <wrl/client.h>
#include <filesystem>
#include <mutex>
#include <vector>
#include <cwchar>

namespace Gaslightr {

namespace {

// ── In-Process WinVerifyTrust Hook for Isolated NGX Loading ──────────────────
typedef LONG(WINAPI* FnWinVerifyTrust)(HWND, GUID*, LPVOID);
static FnWinVerifyTrust g_OrigWinVerifyTrust = nullptr;
static BYTE g_OriginalBytes[14] = {0};
static std::mutex g_HookMutex;
static std::atomic<bool> g_HookInstalled{false};

static bool IsTargetDlssBinary(LPCWSTR filePath) {
    if (!filePath) return false;
    wchar_t lower[MAX_PATH] = {0};
    wcsncpy_s(lower, filePath, _TRUNCATE);
    _wcslwr_s(lower);
    return (wcsstr(lower, L"nvngx_dlssnr.dll") != nullptr ||
            wcsstr(lower, L"nvngx_dlssd.dll") != nullptr ||
            wcsstr(lower, L"nvngx_dlss.dll") != nullptr);
}

static LONG WINAPI HookedWinVerifyTrust(HWND hwnd, GUID* pgActionID, LPVOID pWVTData) {
    auto* pData = reinterpret_cast<WINTRUST_DATA*>(pWVTData);
    if (pData && pData->dwUnionChoice == WTD_CHOICE_FILE && pData->pFile && pData->pFile->pcwszFilePath) {
        if (IsTargetDlssBinary(pData->pFile->pcwszFilePath)) {
            return ERROR_SUCCESS;
        }
    }

    std::lock_guard<std::mutex> lock(g_HookMutex);
    DWORD oldProtect = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), g_OriginalBytes, 14);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14);

    LONG status = g_OrigWinVerifyTrust(hwnd, pgActionID, pWVTData);

    BYTE jmpPatch[14] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    uint64_t targetAddr = reinterpret_cast<uint64_t>(&HookedWinVerifyTrust);
    memcpy(&jmpPatch[6], &targetAddr, sizeof(targetAddr));
    memcpy(reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), jmpPatch, 14);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14);
    VirtualProtect(reinterpret_cast<LPVOID>(g_OrigWinVerifyTrust), 14, oldProtect, &oldProtect);

    return status;
}

static bool EnsureWinVerifyTrustHook() {
    if (g_HookInstalled.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lock(g_HookMutex);
    if (g_HookInstalled.load(std::memory_order_relaxed)) return true;

    HMODULE hWintrust = LoadLibraryW(L"wintrust.dll");
    if (!hWintrust) return false;
    g_OrigWinVerifyTrust = reinterpret_cast<FnWinVerifyTrust>(GetProcAddress(hWintrust, "WinVerifyTrust"));
    if (!g_OrigWinVerifyTrust) return false;

    // If already hooked (first bytes 0xFF 0x25 = jmp qword ptr [rip]), preserve existing hook
    const BYTE* pCode = reinterpret_cast<const BYTE*>(g_OrigWinVerifyTrust);
    if (pCode[0] == 0xFF && pCode[1] == 0x25) {
        g_HookInstalled.store(true, std::memory_order_release);
        return true;
    }

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

    g_HookInstalled.store(true, std::memory_order_release);
    return true;
}

static std::wstring FindDriverStoreTrampoline() {
    const wchar_t* primary = L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_feedb8c0271ca811\\nvngx.dll";
    if (GetFileAttributesW(primary) != INVALID_FILE_ATTRIBUTES) return primary;

    WIN32_FIND_DATAW findData{};
    HANDLE hFind = FindFirstFileW(L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_*", &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring candidate = L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\" + std::wstring(findData.cFileName) + L"\\nvngx.dll";
                if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    FindClose(hFind);
                    return candidate;
                }
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
    return L"nvngx.dll";
}

static std::wstring FindSnippetDirectory() {
    const wchar_t* relVendor = L"vendor\\nvidia\\incoming";
    if (GetFileAttributesW(relVendor) != INVALID_FILE_ATTRIBUTES) {
        return std::filesystem::absolute(relVendor).wstring();
    }
    const wchar_t* localPath = L"nvngx_dlssd.dll";
    if (GetFileAttributesW(localPath) != INVALID_FILE_ATTRIBUTES) {
        return std::filesystem::current_path().wstring();
    }
    return L".";
}

} // namespace

struct NeuralBackend::NgxSession {
    HMODULE hTrampoline = nullptr;
    ID3D12Device* pDevice = nullptr;
    PFN_NVSDK_NGX_D3D12_Init_ProjectID pfnInitProjectID = nullptr;
    PFN_NVSDK_NGX_D3D12_Init_Ext pfnInitExt = nullptr;
    PFN_NVSDK_NGX_D3D12_AllocateParameters pfnAllocateParameters = nullptr;
    PFN_NVSDK_NGX_D3D12_DestroyParameters pfnDestroyParameters = nullptr;
    PFN_NVSDK_NGX_D3D12_CreateFeature pfnCreateFeature = nullptr;
    PFN_NVSDK_NGX_D3D12_EvaluateFeature pfnEvaluateFeature = nullptr;
    PFN_NVSDK_NGX_D3D12_ReleaseFeature pfnReleaseFeature = nullptr;
    PFN_NVSDK_NGX_D3D12_Shutdown pfnShutdown = nullptr;
    PFN_NVSDK_NGX_D3D12_Shutdown1 pfnShutdown1 = nullptr;

    NVSDK_NGX_Parameter* pParams = nullptr;
    void* pFeatureHandle = nullptr;
    uint32_t inW = 0;
    uint32_t inH = 0;
    uint32_t outW = 0;
    uint32_t outH = 0;
    bool isRayReconstruction = false;
    int quality = 2;

    ~NgxSession() {
        if (pfnReleaseFeature && pFeatureHandle) {
            pfnReleaseFeature(pFeatureHandle);
            pFeatureHandle = nullptr;
        }
        if (pfnDestroyParameters && pParams) {
            pfnDestroyParameters(pParams);
            pParams = nullptr;
        }
        if (pfnShutdown && pDevice) {
            pfnShutdown(pDevice);
        } else if (pfnShutdown1) {
            pfnShutdown1();
        }
        if (hTrampoline) {
            FreeLibrary(hTrampoline);
            hTrampoline = nullptr;
        }
    }
};

NeuralBackend& NeuralBackend::GetInstance() {
    static NeuralBackend instance;
    return instance;
}

bool NeuralBackend::Initialize(const wchar_t*, ID3D12Device* device, NVNGX_InitializationParams* parameters) {
    if (!kNeuralInferenceAvailable) {
        Shutdown();
        m_lastError = L"DLSS-NR ABI is unverified. Use the explicit D3D12 validation-copy pipeline.";
        return false;
    }

    if (!device || !parameters) {
        m_lastError = L"Invalid initialization parameters.";
        return false;
    }

    auto res = InitializeExplicit(device, parameters->Width, parameters->Height,
        parameters->TargetWidth, parameters->TargetHeight, true, parameters->PerformanceMode);
    if (!res) {
        m_lastError = res.detail;
        return false;
    }
    return true;
}

DX12Result NeuralBackend::InitializeExplicit(ID3D12Device* device, uint32_t inWidth, uint32_t inHeight,
    uint32_t outWidth, uint32_t outHeight, bool enableRayReconstruction, int perfQuality) {
    Shutdown();

    if (!device || inWidth == 0 || inHeight == 0 || outWidth == 0 || outHeight == 0) {
        return DX12Result::Fail(E_INVALIDARG, L"Invalid dimensions or device pointer for NGX initialization.");
    }

    EnsureWinVerifyTrustHook();

    auto session = std::make_unique<NgxSession>();
    session->inW = inWidth;
    session->inH = inHeight;
    session->outW = outWidth;
    session->outH = outHeight;
    session->isRayReconstruction = enableRayReconstruction;
    session->quality = perfQuality;

    const std::wstring trampolinePath = FindDriverStoreTrampoline();
    session->hTrampoline = LoadLibraryExW(trampolinePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!session->hTrampoline) {
        session->hTrampoline = LoadLibraryExW(L"nvngx.dll", nullptr, 0);
    }
    if (!session->hTrampoline) {
        DWORD err = ::GetLastError();
        return DX12Result::Fail(HRESULT_FROM_WIN32(err), L"Failed to load NVIDIA NGX trampoline (nvngx.dll).");
    }

    session->pfnInitProjectID = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Init_ProjectID>(
        GetProcAddress(session->hTrampoline, "NVSDK_NGX_D3D12_Init_ProjectID"));
    session->pfnInitExt = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Init_Ext>(
        GetProcAddress(session->hTrampoline, "NVSDK_NGX_D3D12_Init_Ext"));
    session->pfnAllocateParameters = reinterpret_cast<PFN_NVSDK_NGX_D3D12_AllocateParameters>(
        GetProcAddress(session->hTrampoline, "NVSDK_NGX_D3D12_AllocateParameters"));
    session->pfnDestroyParameters = reinterpret_cast<PFN_NVSDK_NGX_D3D12_DestroyParameters>(
        GetProcAddress(session->hTrampoline, "NVSDK_NGX_D3D12_DestroyParameters"));
    session->pfnCreateFeature = reinterpret_cast<PFN_NVSDK_NGX_D3D12_CreateFeature>(
        GetProcAddress(session->hTrampoline, "NVSDK_NGX_D3D12_CreateFeature"));
    session->pfnEvaluateFeature = reinterpret_cast<PFN_NVSDK_NGX_D3D12_EvaluateFeature>(
        GetProcAddress(session->hTrampoline, "NVSDK_NGX_D3D12_EvaluateFeature"));
    session->pfnReleaseFeature = reinterpret_cast<PFN_NVSDK_NGX_D3D12_ReleaseFeature>(
        GetProcAddress(session->hTrampoline, "NVSDK_NGX_D3D12_ReleaseFeature"));
    session->pDevice = device;
    session->pfnShutdown = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Shutdown>(
        GetProcAddress(session->hTrampoline, "NVSDK_NGX_D3D12_Shutdown"));
    session->pfnShutdown1 = reinterpret_cast<PFN_NVSDK_NGX_D3D12_Shutdown1>(
        GetProcAddress(session->hTrampoline, "NVSDK_NGX_D3D12_Shutdown1"));

    if (!session->pfnAllocateParameters || !session->pfnCreateFeature || !session->pfnEvaluateFeature) {
        return DX12Result::Fail(E_FAIL, L"Missing essential NVSDK_NGX entry points in nvngx.dll.");
    }

    // 1. Initialize NGX
    std::wstring snippetDir = FindSnippetDirectory();
    const wchar_t* searchPaths[] = { snippetDir.c_str() };
    NVSDK_NGX_FeatureCommonInfo commonInfo{};
    commonInfo.PathListInfo.Path = searchPaths;
    commonInfo.PathListInfo.Length = 1;

    NVSDK_NGX_Result initRes = 0;
    const char* kGaslightrGuid = "24e93019-9430-4e13-94c6-e794b54e38c3";
    if (session->pfnInitProjectID) {
        initRes = session->pfnInitProjectID(kGaslightrGuid, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
            snippetDir.c_str(), device, 0x14, &commonInfo);
        if (initRes != NVSDK_NGX_Result_Success) {
            initRes = session->pfnInitProjectID(kGaslightrGuid, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                snippetDir.c_str(), device, 0x13, &commonInfo);
        }
    } else if (session->pfnInitExt) {
        initRes = session->pfnInitExt(0x0E658703, snippetDir.c_str(), device, 0x14, &commonInfo);
        if (initRes != NVSDK_NGX_Result_Success) {
            initRes = session->pfnInitExt(0x0E658703, snippetDir.c_str(), device, 0x13, &commonInfo);
        }
    }
    if (initRes != NVSDK_NGX_Result_Success) {
        return DX12Result::Fail(E_FAIL, L"NVSDK_NGX_D3D12_Init failed: " + std::to_wstring(initRes));
    }

    // 2. Allocate Parameters
    void* rawParams = nullptr;
    NVSDK_NGX_Result allocRes = session->pfnAllocateParameters(&rawParams);
    if (allocRes != NVSDK_NGX_Result_Success || !rawParams) {
        return DX12Result::Fail(E_FAIL, L"NVSDK_NGX_D3D12_AllocateParameters failed: " + std::to_wstring(allocRes));
    }
    session->pParams = reinterpret_cast<NVSDK_NGX_Parameter*>(rawParams);

    // 3. Configure Creation Parameters
    session->pParams->Reset();
    session->pParams->Set("Width", inWidth);
    session->pParams->Set("Height", inHeight);
    session->pParams->Set("OutWidth", outWidth);
    session->pParams->Set("OutHeight", outHeight);
    session->pParams->Set("PerfQualityValue", static_cast<unsigned int>(perfQuality));
    session->pParams->Set("RTXValue", 1u);
    session->pParams->Set("Reset", 1u);

    if (enableRayReconstruction) {
        session->pParams->Set("DLSS.Feature.Create.Flags", 0u);
        session->pParams->Set("DLSS.Denoise.Mode", 1u); // DLUnified mode
        session->pParams->Set("DLSS.Roughness.Mode", 0u);
        session->pParams->Set("DLSS.Render.Subrect.Dimensions.Width", inWidth);
        session->pParams->Set("DLSS.Render.Subrect.Dimensions.Height", inHeight);
    }

    // 4. Create Feature 1 (SuperSampling / DLSS-D Ray Reconstruction)
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> initQueue;
    HRESULT hrQueue = device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&initQueue));
    if (FAILED(hrQueue)) {
        return DX12Result::Fail(hrQueue, L"Failed to create command queue for NGX feature creation.");
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> initAlloc;
    HRESULT hrAlloc = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&initAlloc));
    if (FAILED(hrAlloc)) {
        return DX12Result::Fail(hrAlloc, L"Failed to create command allocator for NGX feature creation.");
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> initCmdList;
    HRESULT hrList = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, initAlloc.Get(), nullptr, IID_PPV_ARGS(&initCmdList));
    if (FAILED(hrList)) {
        return DX12Result::Fail(hrList, L"Failed to create command list for NGX feature creation.");
    }

    NVSDK_NGX_Result createRes = session->pfnCreateFeature(initCmdList.Get(), NVSDK_NGX_Feature_SuperSampling,
        session->pParams, &session->pFeatureHandle);
    if (createRes != NVSDK_NGX_Result_Success || !session->pFeatureHandle) {
        return DX12Result::Fail(E_FAIL, L"NVSDK_NGX_D3D12_CreateFeature failed: " + std::to_wstring(createRes));
    }

    initCmdList->Close();
    ID3D12CommandList* cmdLists[] = { initCmdList.Get() };
    initQueue->ExecuteCommandLists(1, cmdLists);

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        initQueue->Signal(fence.Get(), 1);
        HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (hEvent) {
            fence->SetEventOnCompletion(1, hEvent);
            WaitForSingleObject(hEvent, INFINITE);
            CloseHandle(hEvent);
        }
    }

    m_session = std::move(session);
    m_loaded.store(true, std::memory_order_release);
    m_ready.store(true, std::memory_order_release);
    m_lastError.clear();

    return DX12Result::Success();
}

DX12Result NeuralBackend::Evaluate(ID3D12GraphicsCommandList* commandList,
    NVNGX_EvaluationInputs* inputs, ID3D12Resource* output) {
    if (!m_ready.load(std::memory_order_acquire) || !m_session) {
        return DX12Result::Fail(E_NOTIMPL,
            L"Neural inference is disabled: no verified DLSS-NR integration contract.");
    }
    if (!inputs || !output) {
        return DX12Result::Fail(E_INVALIDARG, L"Invalid inputs or output resource for NGX evaluation.");
    }
    return EvaluateExplicit(commandList, *inputs, output);
}

DX12Result NeuralBackend::EvaluateExplicit(ID3D12GraphicsCommandList* commandList,
    const NVNGX_EvaluationInputs& inputs, ID3D12Resource* output) {
    if (!m_session || !m_session->pFeatureHandle || !m_session->pParams || !m_session->pfnEvaluateFeature) {
        return DX12Result::Fail(E_NOT_VALID_STATE, L"NGX session is not initialized or ready.");
    }
    if (!commandList || !inputs.pColorInput || !output) {
        return DX12Result::Fail(E_INVALIDARG, L"Invalid command list, color input, or output resource.");
    }

    NVSDK_NGX_Parameter* pParams = m_session->pParams;
    pParams->Set("Color", inputs.pColorInput);
    pParams->Set("Output", output);
    if (inputs.pDepthBuffer)     pParams->Set("Depth", inputs.pDepthBuffer);
    if (inputs.pMotionVectors)   pParams->Set("MotionVectors", inputs.pMotionVectors);
    if (inputs.pNormals)         pParams->Set("DLSS.Input.Normals", inputs.pNormals);
    if (inputs.pRoughness)       pParams->Set("DLSS.Input.Roughness", inputs.pRoughness);
    if (inputs.pDiffuseAlbedo)   pParams->Set("DLSS.Input.DiffuseAlbedo", inputs.pDiffuseAlbedo);
    if (inputs.pSpecularAlbedo)  pParams->Set("DLSS.Input.SpecularAlbedo", inputs.pSpecularAlbedo);

    pParams->Set("Jitter.Offset.X", inputs.CameraJitterX);
    pParams->Set("Jitter.Offset.Y", inputs.CameraJitterY);
    pParams->Set("Sharpness", 0.0f);
    pParams->Set("Reset", inputs.ResetHistory ? 1 : 0);
    pParams->Set("MV.Scale.X", inputs.MotionVectorScaleX);
    pParams->Set("MV.Scale.Y", inputs.MotionVectorScaleY);
    pParams->Set("MV.Offset.X", 0.0f);
    pParams->Set("MV.Offset.Y", 0.0f);
    pParams->Set("RenderSubrectDimensionsWidth", m_session->inW);
    pParams->Set("RenderSubrectDimensionsHeight", m_session->inH);
    pParams->Set("PreExposure", 1.0f);
    pParams->Set("ExposureScale", 1.0f);

    NVSDK_NGX_Result evalRes = m_session->pfnEvaluateFeature(commandList,
        m_session->pFeatureHandle, pParams, nullptr);
    if (evalRes != NVSDK_NGX_Result_Success) {
        return DX12Result::Fail(E_FAIL, L"NVSDK_NGX_D3D12_EvaluateFeature failed: " + std::to_wstring(evalRes));
    }

    return DX12Result::Success();
}

void NeuralBackend::Shutdown() {
    m_ready.store(false, std::memory_order_release);
    m_loaded.store(false, std::memory_order_release);
    m_session.reset();
    m_lastError.clear();
}

NeuralBackend::~NeuralBackend() {
    Shutdown();
}

} // namespace Gaslightr
