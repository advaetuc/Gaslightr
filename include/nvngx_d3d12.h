#pragma once
// =============================================================================
// Gaslightr — nvngx_d3d12.h
// Verified NVIDIA NGX Direct3D 12 ABI, data structures, and parameter interfaces.
// Exact reverse-engineered vtable layout matching _nvngx.dll RVA 0xB59B8.
// =============================================================================

#include <windows.h>
#include <d3d12.h>
#include <cstdint>

namespace Gaslightr {

using NVSDK_NGX_Result = uint32_t;

inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Success                                  = 0x00000001;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_FeatureNotSupported                 = 0xBAD00000;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_PlatformError                       = 0xBAD00002;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_FeatureAlreadyExists                = 0xBAD00003;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_FeatureNotFound                     = 0xBAD00004;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_InvalidParameter                   = 0xBAD00005;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_ScratchBufferTooSmall              = 0xBAD00006;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_NotInitialized                      = 0xBAD00007;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_UnsupportedInputFormat              = 0xBAD00008;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_RWFlagMissing                       = 0xBAD00009;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_MissingInput                       = 0xBAD0000A;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_UnableToEvaluateFeature             = 0xBAD0000B;
inline constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Fail_InvalidParamValue                  = 0xBAD00010;

enum NVSDK_NGX_Logging_Level : uint32_t {
    NVSDK_NGX_LOGGING_LEVEL_OFF     = 0,
    NVSDK_NGX_LOGGING_LEVEL_ON      = 1,
    NVSDK_NGX_LOGGING_LEVEL_VERBOSE = 2
};

enum NVSDK_NGX_Feature : uint32_t {
    NVSDK_NGX_Feature_Reserved_Zero        = 0,
    NVSDK_NGX_Feature_SuperSampling        = 1, // DLSS / DLSS-D Ray Reconstruction
    NVSDK_NGX_Feature_InPainting           = 2,
    NVSDK_NGX_Feature_ImageSuperResolution = 3,
    NVSDK_NGX_Feature_SlowMotion           = 4,
    NVSDK_NGX_Feature_VideoSuperResolution = 5,
    NVSDK_NGX_Feature_RayReconstruction    = 13, // Legacy standalone snippet Feature ID
    NVSDK_NGX_Feature_Count                = 14
};

enum NVSDK_NGX_PerfQuality_Value : uint32_t {
    NVSDK_NGX_PerfQuality_Value_MaxPerf          = 0,
    NVSDK_NGX_PerfQuality_Value_Balanced         = 1,
    NVSDK_NGX_PerfQuality_Value_MaxQuality       = 2,
    NVSDK_NGX_PerfQuality_Value_UltraPerformance = 3,
    NVSDK_NGX_PerfQuality_Value_UltraQuality     = 4,
    NVSDK_NGX_PerfQuality_Value_DLAA             = 5
};

enum NVSDK_NGX_DLSS_Denoise_Mode : uint32_t {
    NVSDK_NGX_DLSS_Denoise_Mode_Off       = 0,
    NVSDK_NGX_DLSS_Denoise_Mode_DLUnified = 1
};

enum NVSDK_NGX_EngineType : uint32_t {
    NVSDK_NGX_ENGINE_TYPE_CUSTOM    = 0,
    NVSDK_NGX_ENGINE_TYPE_UNREAL    = 1,
    NVSDK_NGX_ENGINE_TYPE_UNITY     = 2,
    NVSDK_NGX_ENGINE_TYPE_OMNIVERSE = 3
};

using NVSDK_NGX_Version = uint32_t;

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

struct ID3D11Resource;

// Polymorphic Parameter Map matching _nvngx.dll vtable at RVA 0xB59B8
struct NVSDK_NGX_Parameter {
    virtual void SetU64(const char* InName, unsigned long long InValue) = 0; // [0]
    virtual void SetD3D12(const char* InName, ID3D12Resource* InValue) = 0;  // [1]
    virtual void SetD3D11(const char* InName, ID3D11Resource* InValue) = 0;  // [2]
    virtual void SetI(const char* InName, int InValue) = 0;                  // [3]
    virtual void SetUI(const char* InName, unsigned int InValue) = 0;        // [4]
    virtual void SetD(const char* InName, double InValue) = 0;               // [5]
    virtual void SetF(const char* InName, float InValue) = 0;                // [6]
    virtual void SetVoid(const char* InName, void* InValue) = 0;             // [7]

    virtual NVSDK_NGX_Result GetD3D12(const char* InName, ID3D12Resource** OutValue) = 0;    // [8]
    virtual NVSDK_NGX_Result GetVoid(const char* InName, void** OutValue) = 0;              // [9]
    virtual NVSDK_NGX_Result GetD3D11(const char* InName, ID3D11Resource** OutValue) = 0;    // [10]
    virtual NVSDK_NGX_Result GetI(const char* InName, int* OutValue) = 0;                    // [11]
    virtual NVSDK_NGX_Result GetUI(const char* InName, unsigned int* OutValue) = 0;          // [12]
    virtual NVSDK_NGX_Result GetD(const char* InName, double* OutValue) = 0;                 // [13]
    virtual NVSDK_NGX_Result GetF(const char* InName, float* OutValue) = 0;                  // [14]
    virtual NVSDK_NGX_Result GetU64(const char* InName, unsigned long long* OutValue) = 0;   // [15]

    virtual void Reset() = 0;                                                                // [16]

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

// ── Entry Point Typedefs ─────────────────────────────────────────────────────

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

using PFN_NVSDK_NGX_D3D12_Shutdown = NVSDK_NGX_Result(__cdecl *)(ID3D12Device* InDevice);
using PFN_NVSDK_NGX_D3D12_Shutdown1 = NVSDK_NGX_Result(__cdecl *)(void);

inline const char* GetNgxResultString(NVSDK_NGX_Result res) noexcept {
    switch (res) {
        case NVSDK_NGX_Result_Success: return "NVSDK_NGX_Result_Success";
        case NVSDK_NGX_Result_Fail_FeatureNotSupported: return "NVSDK_NGX_Result_Fail_FeatureNotSupported";
        case NVSDK_NGX_Result_Fail_PlatformError: return "NVSDK_NGX_Result_Fail_PlatformError";
        case NVSDK_NGX_Result_Fail_FeatureAlreadyExists: return "NVSDK_NGX_Result_Fail_FeatureAlreadyExists";
        case NVSDK_NGX_Result_Fail_FeatureNotFound: return "NVSDK_NGX_Result_Fail_FeatureNotFound";
        case NVSDK_NGX_Result_Fail_InvalidParameter: return "NVSDK_NGX_Result_Fail_InvalidParameter";
        case NVSDK_NGX_Result_Fail_ScratchBufferTooSmall: return "NVSDK_NGX_Result_Fail_ScratchBufferTooSmall";
        case NVSDK_NGX_Result_Fail_NotInitialized: return "NVSDK_NGX_Result_Fail_NotInitialized";
        case NVSDK_NGX_Result_Fail_UnsupportedInputFormat: return "NVSDK_NGX_Result_Fail_UnsupportedInputFormat";
        case NVSDK_NGX_Result_Fail_RWFlagMissing: return "NVSDK_NGX_Result_Fail_RWFlagMissing";
        case NVSDK_NGX_Result_Fail_MissingInput: return "NVSDK_NGX_Result_Fail_MissingInput";
        case NVSDK_NGX_Result_Fail_UnableToEvaluateFeature: return "NVSDK_NGX_Result_Fail_UnableToEvaluateFeature";
        case NVSDK_NGX_Result_Fail_InvalidParamValue: return "NVSDK_NGX_Result_Fail_InvalidParamValue";
        default: return "NVSDK_NGX_Result_Unknown";
    }
}

} // namespace Gaslightr
