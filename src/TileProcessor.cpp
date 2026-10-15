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
void TransitionBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after || !res || !list) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = res;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
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
    config.validationOnly = request.validationOnly;
    config.intensity = request.intensity;
    config.structure = request.structure;
    Begin(context);
    Require(memory.UploadToGPU(tile, rgba.data(), request.width * 4 * sizeof(float), SourcePixelFormat::RGBA_32F, context.GetCommandList()));
    Submit(context);

    ID3D12Resource* output = nullptr;
    bool neuralRan = false;

    if (!request.validationOnly && request.executionMode == ExecutionMode::Neural && kNeuralInferenceAvailable) {
        try {
            OutputDebugStringA("[Gaslightr] Dispatching NeuralBackend DLSS-D Ray Reconstruction...\n");
            auto initRes = backend.InitializeExplicit(context.GetDevice(), request.width, request.height,
                request.width, request.height, true, request.preset);
            if (initRes && backend.IsReady()) {
                const auto& synth = simulator.GetSyntheticResources();
                NVNGX_EvaluationInputs evalInputs{};
                evalInputs.pColorInput = tile.gpuTexture.Get();
                evalInputs.pDepthBuffer = synth.depthBuffer.Get();
                evalInputs.pMotionVectors = synth.motionVectors.Get();
                evalInputs.pNormals = synth.normalsTexture.Get();
                evalInputs.pRoughness = synth.roughnessTexture.Get();
                evalInputs.pDiffuseAlbedo = tile.gpuTexture.Get();
                evalInputs.pSpecularAlbedo = synth.specularAlbedoTexture.Get();
                evalInputs.MotionVectorScaleX = 1.0f;
                evalInputs.MotionVectorScaleY = 1.0f;
                evalInputs.CameraJitterX = 0.0f;
                evalInputs.CameraJitterY = 0.0f;
                evalInputs.FrameIndex = 0;
                evalInputs.ResetHistory = true;
                evalInputs.SubrectOffsetX = static_cast<uint32_t>(std::max(0, request.tileX));
                evalInputs.SubrectOffsetY = static_cast<uint32_t>(std::max(0, request.tileY));
                evalInputs.CanvasWidth = request.canvasWidth;
                evalInputs.CanvasHeight = request.canvasHeight;

                Begin(context);
                TransitionBarrier(context.GetCommandList(), synth.pingTexture.Get(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                auto evalRes = backend.EvaluateExplicit(context.GetCommandList(), evalInputs, synth.pingTexture.Get());
                if (evalRes) {
                    const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                        | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    TransitionBarrier(context.GetCommandList(), synth.pingTexture.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable);
                    Submit(context);
                    output = synth.pingTexture.Get();
                    neuralRan = true;
                    OutputDebugStringA("[Gaslightr] NeuralBackend DLSS-D Ray Reconstruction evaluated cleanly.\n");
                } else {
                    TransitionBarrier(context.GetCommandList(), synth.pingTexture.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                    context.GetCommandList()->Close();
                    context.WaitForGPU();
                    OutputDebugStringA("[Gaslightr] NeuralBackend::EvaluateExplicit failed; falling back to compute shader.\n");
                }
            } else {
                OutputDebugStringA("[Gaslightr] NeuralBackend::InitializeExplicit unready; falling back to compute shader.\n");
            }
        } catch (const std::exception& e) {
            OutputDebugStringA("[Gaslightr] Neural exception: ");
            OutputDebugStringA(e.what());
            OutputDebugStringA("; falling back to compute shader.\n");
        } catch (...) {
            OutputDebugStringA("[Gaslightr] Unknown neural failure; falling back to compute shader.\n");
        }
    }

    if (!neuralRan) {
        if (!config.validationOnly) {
            config.useComputePipeline = true;
        }
        Require(simulator.ExecuteFullPipeline(tile.gpuTexture.Get(), &output, config));
    }
    // Output is guaranteed shader-readable; MemoryBridge performs the
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
    Require(context.WaitForGPU());
    return result;
}
}
