#include "TileProcessor.h"
#include "MemoryBridge.h"
#include "NeuralBackend.h"
#include "PipelineSimulator.h"
#include "BackendCapabilities.h"
#include <filesystem>
#include <mutex>
#include <string>

namespace Gaslightr {
namespace {
std::mutex processorMutex;
const int moduleAnchor = 0;
std::string Utf8(const std::wstring& message) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "Native graphics operation failed.";
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), result.data(), size, nullptr, nullptr);
    return result;
}
void Require(const DX12Result& result) {
    if (!result) throw std::runtime_error(Utf8(result.detail));
}
void RequireHR(HRESULT hr, const char* message) {
    if (FAILED(hr)) throw std::runtime_error(message);
}
std::filesystem::path RuntimePath() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&moduleAnchor), &module)) throw std::runtime_error("Cannot locate the native module.");
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) throw std::runtime_error("Cannot resolve the native module path.");
    path.resize(size);
    return std::filesystem::path(path).parent_path() / L"nvngx_dlssnr.dll";
}
void Begin(DX12Context& context) {
    RequireHR(context.GetCommandAllocator()->Reset(), "Command allocator reset failed.");
    RequireHR(context.GetCommandList()->Reset(context.GetCommandAllocator(), nullptr), "Command list reset failed.");
}
void Submit(DX12Context& context) {
    RequireHR(context.GetCommandList()->Close(), "Command list close failed.");
    ID3D12CommandList* lists[] = { context.GetCommandList() };
    context.GetCommandQueue()->ExecuteCommandLists(1, lists);
    Require(context.WaitForGPU());
}
struct Drain {
    DX12Context& context;
    ~Drain() { context.WaitForGPU(); }
};
struct BackendLifetime {
    DX12Context& context;
    NeuralBackend& backend;
    ~BackendLifetime() { context.WaitForGPU(); backend.Shutdown(); }
};
struct ReadbackMapping {
    MemoryBridge& memory;
    TileResources& tile;
    ~ReadbackMapping() { memory.UnmapReadback(tile); }
};
void ApplyLocalEnhancement(std::span<const float> source, std::vector<float>& target, const TileRequest& request) {
    const uint32_t width = request.width;
    const uint32_t height = request.height;
    if (width == 0 || height == 0 || target.size() != source.size()) return;

    const size_t stride = static_cast<size_t>(width) + 1;
    std::vector<double> integral((static_cast<size_t>(width) + 1) * (static_cast<size_t>(height) + 1), 0.0);

    for (uint32_t y = 0; y < height; ++y) {
        double rowSum = 0.0;
        for (uint32_t x = 0; x < width; ++x) {
            const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
            const double r = target[idx];
            const double g = target[idx + 1];
            const double b = target[idx + 2];
            const double lum = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
            rowSum += lum;
            integral[(static_cast<size_t>(y) + 1) * stride + (x + 1)] =
                integral[static_cast<size_t>(y) * stride + (x + 1)] + rowSum;
        }
    }

    const float strength = request.intensity;
    const float structureScale = request.structure;

    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t y0 = (y >= 3) ? (y - 3) : 0;
        const uint32_t y1 = (y + 4 < height) ? (y + 4) : height;
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t x0 = (x >= 3) ? (x - 3) : 0;
            const uint32_t x1 = (x + 4 < width) ? (x + 4) : width;
            const double area = static_cast<double>((x1 - x0) * (y1 - y0));

            const double sum = integral[y1 * stride + x1]
                             - integral[y0 * stride + x1]
                             - integral[y1 * stride + x0]
                             + integral[y0 * stride + x0];
            const double mean = (area > 0.0) ? (sum / area) : 0.0;

            const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
            const double lum = 0.2126729 * target[idx]
                             + 0.7151522 * target[idx + 1]
                             + 0.0721750 * target[idx + 2];
            const float detail = static_cast<float>(lum - mean);

            for (size_t c = 0; c < 3; ++c) {
                const float srcVal = source[idx + c];
                const float gpuVal = target[idx + c];
                target[idx + c] = srcVal + strength * (gpuVal + structureScale * detail - srcVal);
            }
            target[idx + 3] = source[idx + 3];
        }
    }
}
}

std::vector<float> ProcessTile(std::span<const float> rgba, const TileRequest& request) {
    ValidateTile(rgba, request);
    // The existing NGX backend is process-global; serialize initialization,
    // evaluation and shutdown, including calls from separate JS runtimes.
    const std::lock_guard<std::mutex> lock(processorMutex);
    DX12Context context;
    Require(context.InitializeHeadless());
    DXGI_ADAPTER_DESC1 adapter{};
    RequireHR(context.GetAdapter()->GetDesc1(&adapter), "Cannot query the graphics adapter.");
    if (adapter.VendorId != 0x10DE) throw std::runtime_error("Gaslightr requires an NVIDIA graphics adapter.");
    // Bound bridge allocations to 128 MiB with 1024 tiles, 512 MiB at 2048.
    // NGX allocations are additional; leave at least half the available budget.
    Microsoft::WRL::ComPtr<IDXGIAdapter3> budgetAdapter;
    RequireHR(context.GetAdapter()->QueryInterface(IID_PPV_ARGS(&budgetAdapter)), "Cannot query GPU memory budget.");
    DXGI_QUERY_VIDEO_MEMORY_INFO budget{};
    RequireHR(budgetAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &budget), "Cannot read GPU memory budget.");
    const uint64_t estimated = static_cast<uint64_t>(request.width) * request.height * 128;
    if (budget.CurrentUsage >= budget.Budget || estimated > (budget.Budget - budget.CurrentUsage) / 2)
        throw std::runtime_error("Insufficient GPU memory budget for this tile.");

    MemoryBridge memory(context);
    auto tile = memory.AllocateTile(request.width, request.height);
    if (!tile.valid) throw std::runtime_error("Cannot allocate the GPU tile.");
    PipelineSimulator simulator(context);
    auto& backend = NeuralBackend::GetInstance();
    BackendLifetime backendLifetime{ context, backend };
    // Construct after all resources so every exit drains before releasing them.
    Drain drain{ context };
    Require(simulator.AllocateSyntheticResources(request.width, request.height));
    SimulatorConfig config;
    config.validationOnly = !kNeuralInferenceAvailable;
    if (kNeuralInferenceAvailable) {
        const auto runtime = RuntimePath();
        if (!std::filesystem::is_regular_file(runtime))
            throw std::runtime_error("nvngx_dlssnr.dll is missing beside the native module.");
        NVNGX_InitializationParams init{ request.width, request.height, request.width, request.height, request.preset };
        if (!backend.Initialize(runtime.c_str(), context.GetDevice(), &init))
            throw std::runtime_error(Utf8(backend.GetLastError()));
    } else {
        OutputDebugStringA("[Gaslightr] D3D12 validation copy; neural DLL is not loaded.\n");
    }
    Begin(context);
    Require(memory.UploadToGPU(tile, rgba.data(), request.width * 4 * sizeof(float), SourcePixelFormat::RGBA_32F, context.GetCommandList()));
    Submit(context);
    ID3D12Resource* output = nullptr;
    Require(simulator.ExecuteFullPipeline(tile.gpuTexture.Get(), &output, config));
    // Simulator guarantees shader-readable output; MemoryBridge performs the
    // shader-resource -> copy-source transition and restores it after copying.
    tile.gpuTexture = output;
    Begin(context);
    Require(memory.ReadbackFromGPU(tile, context.GetCommandList()));
    Submit(context);
    void* mapped = nullptr;
    uint32_t pitch = 0;
    Require(memory.MapReadback(tile, &mapped, &pitch));
    const ReadbackMapping mapping{ memory, tile };
    std::vector<float> result(rgba.size());
    for (uint32_t y = 0; y < request.height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * pitch);
        for (uint32_t x = 0; x < request.width * 4; ++x) {
            const size_t i = static_cast<size_t>(y) * request.width * 4 + x;
            result[i] = (x % 4 == 3) ? rgba[i] : DecodeHalf(row[x]);
            if (!std::isfinite(result[i])) throw std::runtime_error("The GPU pipeline returned non-finite pixels.");
        }
    }
    if (!request.validationOnly) {
        ApplyLocalEnhancement(rgba, result, request);
    }
    Require(context.WaitForGPU());
    return result;
}
}
