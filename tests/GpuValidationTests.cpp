#include "TileProcessor.h"
#include "NeuralBackend.h"
#include "BackendCapabilities.h"
#include <array>
#include <cstdio>
#include <utility>
#include <vector>

int main() {
    try {
        if (Gaslightr::kNeuralInferenceAvailable) throw std::runtime_error("Expected validation build.");
        auto& backend = Gaslightr::NeuralBackend::GetInstance();
        if (backend.Initialize(L"nvngx_dlssnr.dll", nullptr, nullptr) || backend.IsLoaded() || backend.IsReady())
            throw std::runtime_error("Unverified backend must remain disabled.");
        // Odd pitch plus all four actual read sizes for a 1024-square canvas.
        const std::array<std::pair<uint32_t, uint32_t>, 5> sizes{{
            {17, 3}, {992, 992}, {96, 992}, {992, 96}, {96, 96}
        }};
        for (const auto& [width, height] : sizes) {
            std::vector<float> input(static_cast<size_t>(width) * height * 4);
            for (size_t i = 0; i < input.size(); i += 4) {
                input[i] = static_cast<float>((i / 4) % 31) * 0.125f;
                input[i + 1] = -0.25f;
                input[i + 2] = ((i / 4) % 2) ? 65504.0f : 4.0f;
                input[i + 3] = static_cast<float>((i / 4) % 11) / 10.0f;
            }
            const auto output = Gaslightr::ProcessTile(input, {width, height, 0, false});
            if (output != input) throw std::runtime_error("GPU copy changed FP16-exact RGB or source alpha.");
            if (GetModuleHandleW(L"nvngx_dlssnr.dll")) throw std::runtime_error("Validation loaded the neural DLL.");
            std::printf("GPU copy passed: %u x %u; alpha preserved; neural DLL not loaded.\n", width, height);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
