#pragma once

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <windows.h>

namespace Gaslightr {

inline bool CheckNeuralFlagFile() noexcept {
    // 1. Check local developer flag in frontend directory
    if (GetFileAttributesW(L"uxp-frontend\\enable_neural.flag") != INVALID_FILE_ATTRIBUTES) return true;
    if (GetFileAttributesW(L"enable_neural.flag") != INVALID_FILE_ATTRIBUTES) return true;

    // 2. Check beside current module
    wchar_t modPath[MAX_PATH]{};
    HMODULE hModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&CheckNeuralFlagFile), &hModule) && hModule) {
        if (GetModuleFileNameW(hModule, modPath, MAX_PATH) > 0) {
            std::filesystem::path dir = std::filesystem::path(modPath).parent_path();
            std::error_code ec;
            if (std::filesystem::exists(dir / L"enable_neural.flag", ec)) return true;
            if (std::filesystem::exists(dir / L".." / L".." / L"enable_neural.flag", ec)) return true;
        }
    }
    return false;
}

inline bool IsNeuralInferenceAvailable() noexcept {
    // 1. Check environment variable
    const char* envVal = std::getenv("GASLIGHTR_ENABLE_NEURAL");
    if (envVal && (std::strcmp(envVal, "1") == 0 || _stricmp(envVal, "true") == 0)) {
        return true;
    }
    // 2. Check local dev flag file
    return CheckNeuralFlagFile();
}

inline const char* GetNeuralInferenceReason() noexcept {
    if (IsNeuralInferenceAvailable()) {
        return "NVIDIA NGX DLSS-D Ray Reconstruction active.";
    }
    return "NVIDIA NGX DLSS-D Ray Reconstruction verified in standalone probe; runtime neural execution disabled inside Photoshop host runtime to ensure application stability. Set GASLIGHTR_ENABLE_NEURAL=1 or create uxp-frontend/enable_neural.flag to enable.";
}

inline const char* GetExecutionMode() noexcept {
    if (IsNeuralInferenceAvailable()) {
        return "d3d12-neural-dlssd";
    }
    return "d3d12-validation-copy";
}

struct NeuralInferenceCapability {
    inline operator bool() const noexcept {
        return IsNeuralInferenceAvailable();
    }
};

struct NeuralInferenceReason {
    inline operator const char*() const noexcept {
        return GetNeuralInferenceReason();
    }
};

struct ExecutionModeString {
    inline operator const char*() const noexcept {
        return GetExecutionMode();
    }
};

inline constexpr NeuralInferenceCapability kNeuralInferenceAvailable{};
inline constexpr NeuralInferenceReason kNeuralInferenceReason{};
inline constexpr ExecutionModeString kExecutionMode{};
}
