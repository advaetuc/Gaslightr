// =============================================================================
// Gaslightr — GpuValidationTests.cpp
// Automated Direct3D 12 Edge-Case Stress & Validation Tests (Phase 8B)
//
// Tests executed directly on discrete NVIDIA GPU (Ada Lovelace / RTX 4080 SUPER):
//   1. D3D12 copy validation & neutral backend verification
//   2. Non-multiple and odd dimensions (1357x891 px, 96x96 px, 17x3 px)
//   3. Floating layers with 100% transparent padding borders (alpha = 0)
//   4. Extreme float values (HDR highlights > 1.0 up to 65504.0, negative floats)
//   5. Tile stitching math & 0-seam boundary invariance
// =============================================================================

#include "TileProcessor.h"
#include "NeuralBackend.h"
#include "BackendCapabilities.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void TestCopyValidation() {
    std::printf("[Phase 8B Test 1] Direct3D 12 Copy Validation...\n");
    if (!Gaslightr::kNeuralInferenceAvailable) {
        auto& backend = Gaslightr::NeuralBackend::GetInstance();
        if (backend.Initialize(L"nvngx_dlssnr.dll", nullptr, nullptr) || backend.IsLoaded() || backend.IsReady()) {
            throw std::runtime_error("Unverified backend must remain disabled.");
        }
    } else {
        std::printf("  [Info] Opt-in Neural Inference capability active.\n");
    }

    const std::array<std::pair<uint32_t, uint32_t>, 5> sizes{{
        {17, 3}, {992, 992}, {96, 992}, {992, 96}, {96, 96}
    }};

    for (const auto& [width, height] : sizes) {
        std::vector<float> input(static_cast<size_t>(width) * height * 4);
        for (size_t i = 0; i < input.size(); i += 4) {
            input[i]     = static_cast<float>((i / 4) % 31) * 0.125f;
            input[i + 1] = -0.25f;
            input[i + 2] = ((i / 4) % 2) ? 65504.0f : 4.0f;
            input[i + 3] = static_cast<float>((i / 4) % 11) / 10.0f;
        }

        Gaslightr::TileRequest request{
            .width = width,
            .height = height,
            .preset = 0,
            .detailOnly = false,
            .intensity = 0.0f,
            .structure = 0.0f,
            .feather = 0.0f,
            .validationOnly = true
        };

        const auto output = Gaslightr::ProcessTile(input, request);
        if (output != input) {
            throw std::runtime_error("GPU copy changed FP16-exact RGB or source alpha.");
        }
        if (GetModuleHandleW(L"nvngx_dlssnr.dll")) {
            throw std::runtime_error("Validation loaded the neural DLL.");
        }
        std::printf("  GPU copy passed: %u x %u; alpha preserved; neural DLL not loaded.\n", width, height);
    }
}

void TestOddAndNonMultipleDimensions() {
    std::printf("[Phase 8B Test 2] Non-Multiple & Odd Dimensions Stress Test...\n");
    const std::array<std::pair<uint32_t, uint32_t>, 3> oddSizes{{
        {1357, 891}, // Large odd, non-power-of-two, non-multiple of 16
        {96, 96},     // Minimum tile dimension
        {17, 3}       // Extreme odd pitch
    }};

    for (const auto& [width, height] : oddSizes) {
        std::vector<float> input(static_cast<size_t>(width) * height * 4);
        for (size_t y = 0; y < height; ++y) {
            for (size_t x = 0; x < width; ++x) {
                const size_t idx = (y * width + x) * 4;
                input[idx]     = static_cast<float>((x * 7 + y * 13) % 256) / 255.0f;
                input[idx + 1] = static_cast<float>((x * 11 + y * 3) % 256) / 255.0f;
                input[idx + 2] = static_cast<float>((x * 5 + y * 17) % 256) / 255.0f;
                input[idx + 3] = 1.0f;
            }
        }

        // 1. Identity test with zero intensity
        Gaslightr::TileRequest reqIdentity{
            .width = width,
            .height = height,
            .preset = 0,
            .detailOnly = false,
            .intensity = 0.0f,
            .structure = 0.7f,
            .feather = 0.0f,
            .validationOnly = false
        };
        const auto outIdentity = Gaslightr::ProcessTile(input, reqIdentity);
        if (outIdentity.size() != input.size()) {
            throw std::runtime_error("Dimension mismatch on identity enhancement.");
        }
        for (size_t i = 0; i < input.size(); ++i) {
            if (std::abs(outIdentity[i] - input[i]) > 1e-3f) {
                throw std::runtime_error("Zero-intensity compute shader altered pixel values.");
            }
        }

        // 2. Active enhancement test
        Gaslightr::TileRequest reqEnhance{
            .width = width,
            .height = height,
            .preset = 0,
            .detailOnly = false,
            .intensity = 0.8f,
            .structure = 0.6f,
            .feather = 0.0f,
            .validationOnly = false
        };
        const auto outEnhance = Gaslightr::ProcessTile(input, reqEnhance);
        for (size_t i = 0; i < input.size(); i += 4) {
            if (!std::isfinite(outEnhance[i]) ||
                !std::isfinite(outEnhance[i + 1]) ||
                !std::isfinite(outEnhance[i + 2])) {
                throw std::runtime_error("Non-finite RGB encountered in odd dimensions.");
            }
            if (outEnhance[i + 3] != input[i + 3]) {
                throw std::runtime_error("Alpha channel altered during odd dimension enhancement.");
            }
        }
        std::printf("  Odd dimension passed: %u x %u (identity and enhancement verified).\n", width, height);
    }
}

void TestFloatingLayerTransparentPadding() {
    std::printf("[Phase 8B Test 3] Floating Layer with 100%% Transparent Borders...\n");
    const uint32_t width = 256;
    const uint32_t height = 256;
    const uint32_t border = 24; // 24px transparent padding around perimeter

    std::vector<float> input(static_cast<size_t>(width) * height * 4, 0.0f);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
            const bool isBorder = (x < border || x >= width - border || y < border || y >= height - border);
            if (isBorder) {
                input[idx]     = 0.0f;
                input[idx + 1] = 0.0f;
                input[idx + 2] = 0.0f;
                input[idx + 3] = 0.0f; // 100% transparent border
            } else {
                input[idx]     = static_cast<float>((x * 3) % 100) / 100.0f;
                input[idx + 1] = static_cast<float>((y * 5) % 100) / 100.0f;
                input[idx + 2] = 0.5f;
                input[idx + 3] = 1.0f; // Opaque core
            }
        }
    }

    Gaslightr::TileRequest request{
        .width = width,
        .height = height,
        .preset = 0,
        .detailOnly = false,
        .intensity = 0.85f,
        .structure = 0.75f,
        .feather = 0.0f,
        .validationOnly = false
    };

    const auto output = Gaslightr::ProcessTile(input, request);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
            const bool isBorder = (x < border || x >= width - border || y < border || y >= height - border);

            if (isBorder) {
                if (output[idx + 3] != 0.0f) {
                    throw std::runtime_error("Transparent border pixel was given non-zero alpha!");
                }
            } else {
                if (output[idx + 3] != 1.0f) {
                    throw std::runtime_error("Opaque core pixel lost alpha!");
                }
            }

            for (size_t c = 0; c < 3; ++c) {
                if (!std::isfinite(output[idx + c])) {
                    throw std::runtime_error("Non-finite pixel found in floating layer test.");
                }
            }
        }
    }

    std::printf("  Transparent padding test passed: %u x %u (border alpha strictly 0.0, core alpha 1.0).\n", width, height);
}

void TestExtremeFloatAndHdrValues() {
    std::printf("[Phase 8B Test 4] Extreme Float & HDR Values Test...\n");
    const uint32_t width = 64;
    const uint32_t height = 64;

    std::vector<float> input(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < input.size(); i += 4) {
        const size_t px = i / 4;
        if (px % 4 == 0) {
            // Extreme HDR highlight (FP16 max)
            input[i]     = 65504.0f;
            input[i + 1] = 12000.0f;
            input[i + 2] = 2500.0f;
        } else if (px % 4 == 1) {
            // Moderate HDR highlight (> 1.0) with FP16-representable fractions
            input[i]     = 3.5f;
            input[i + 1] = 8.25f;
            input[i + 2] = 1.75f;
        } else if (px % 4 == 2) {
            // Negative linear float values
            input[i]     = -0.5f;
            input[i + 1] = -2.0f;
            input[i + 2] = -0.125f;
        } else {
            // Standard SDR range
            input[i]     = 0.5f;
            input[i + 1] = 0.25f;
            input[i + 2] = 0.75f;
        }
        input[i + 3] = (px % 3 == 0) ? 0.3f : 1.0f;
    }

    // 1. Validation copy path
    {
        Gaslightr::TileRequest req{
            .width = width, .height = height, .preset = 0,
            .intensity = 0.0f, .structure = 0.0f, .validationOnly = true
        };
        const auto out = Gaslightr::ProcessTile(input, req);
        if (out != input) {
            throw std::runtime_error("Validation copy modified HDR or negative float values!");
        }
    }

    // 2. Compute Shader path with active enhancement
    {
        Gaslightr::TileRequest req{
            .width = width, .height = height, .preset = 0,
            .intensity = 0.6f, .structure = 0.5f, .validationOnly = false
        };
        const auto out = Gaslightr::ProcessTile(input, req);
        for (size_t i = 0; i < input.size(); i += 4) {
            for (size_t c = 0; c < 3; ++c) {
                if (!std::isfinite(out[i + c])) {
                    throw std::runtime_error("Compute shader produced non-finite HDR pixel!");
                }
            }
            if (out[i + 3] != input[i + 3]) {
                throw std::runtime_error("Compute shader altered alpha on HDR pixel!");
            }
        }
    }

    std::printf("  Extreme float & HDR test passed: 65504.0 max highlight and negative floats preserved.\n");
}

void TestTileStitchingSeamInvariance() {
    std::printf("[Phase 8B Test 5] Tile Stitching Math & 0-Seam Boundary Invariance...\n");
    const uint32_t canvasWidth = 1357;
    const uint32_t canvasHeight = 891;
    const uint32_t step = 960;
    const uint32_t halo = 16; // Halo padding >= 3px filter radius

    // 1. Mathematical partitioning invariant: every pixel is covered exactly once
    std::vector<uint8_t> coverage(static_cast<size_t>(canvasWidth) * canvasHeight, 0);

    for (uint32_t top = 0; top < canvasHeight; top += step) {
        for (uint32_t left = 0; left < canvasWidth; left += step) {
            const uint32_t coreRight = std::min(canvasWidth, left + step);
            const uint32_t coreBottom = std::min(canvasHeight, top + step);

            for (uint32_t y = top; y < coreBottom; ++y) {
                for (uint32_t x = left; x < coreRight; ++x) {
                    const size_t idx = static_cast<size_t>(y) * canvasWidth + x;
                    if (coverage[idx] != 0) {
                        throw std::runtime_error("Tile stitching overlap detected: pixel covered twice!");
                    }
                    coverage[idx] = 1;
                }
            }
        }
    }

    for (size_t i = 0; i < coverage.size(); ++i) {
        if (coverage[i] != 1) {
            throw std::runtime_error("Tile stitching coverage gap: pixel omitted in partition!");
        }
    }

    // 2. Numerical 0-seam invariance test across odd boundary
    // Test region spanning two adjacent horizontal tiles with an odd split
    const uint32_t testW = 200;
    const uint32_t testH = 64;
    const uint32_t splitX = 97; // Odd boundary column

    std::vector<float> fullImage(static_cast<size_t>(testW) * testH * 4);
    for (uint32_t y = 0; y < testH; ++y) {
        for (uint32_t x = 0; x < testW; ++x) {
            const size_t idx = (static_cast<size_t>(y) * testW + x) * 4;
            fullImage[idx]     = static_cast<float>((x * 19 + y * 7) % 256) / 255.0f;
            fullImage[idx + 1] = static_cast<float>((x * 3 + y * 23) % 256) / 255.0f;
            fullImage[idx + 2] = static_cast<float>((x * 11 + y * 13) % 256) / 255.0f;
            fullImage[idx + 3] = 1.0f;
        }
    }

    // Ground truth: continuous evaluation of the entire strip
    Gaslightr::TileRequest reqFull{
        .width = testW, .height = testH, .preset = 0,
        .intensity = 0.8f, .structure = 0.6f, .validationOnly = false
    };
    const auto groundTruth = Gaslightr::ProcessTile(fullImage, reqFull);

    // Stitched evaluation:
    // Tile 1: covers core [0, splitX), with halo right: read [0, splitX + halo]
    const uint32_t t1ReadW = splitX + halo;
    std::vector<float> t1Input(static_cast<size_t>(t1ReadW) * testH * 4);
    for (uint32_t y = 0; y < testH; ++y) {
        for (uint32_t x = 0; x < t1ReadW; ++x) {
            const size_t dst = (static_cast<size_t>(y) * t1ReadW + x) * 4;
            const size_t src = (static_cast<size_t>(y) * testW + x) * 4;
            for (size_t c = 0; c < 4; ++c) t1Input[dst + c] = fullImage[src + c];
        }
    }
    Gaslightr::TileRequest reqT1{
        .width = t1ReadW, .height = testH, .preset = 0,
        .intensity = 0.8f, .structure = 0.6f, .validationOnly = false
    };
    const auto t1Output = Gaslightr::ProcessTile(t1Input, reqT1);

    // Tile 2: covers core [splitX, testW), with halo left: read [splitX - halo, testW]
    const uint32_t t2ReadW = (testW - splitX) + halo;
    std::vector<float> t2Input(static_cast<size_t>(t2ReadW) * testH * 4);
    for (uint32_t y = 0; y < testH; ++y) {
        for (uint32_t x = 0; x < t2ReadW; ++x) {
            const size_t dst = (static_cast<size_t>(y) * t2ReadW + x) * 4;
            const size_t src = (static_cast<size_t>(y) * testW + (splitX - halo + x)) * 4;
            for (size_t c = 0; c < 4; ++c) t2Input[dst + c] = fullImage[src + c];
        }
    }
    Gaslightr::TileRequest reqT2{
        .width = t2ReadW, .height = testH, .preset = 0,
        .intensity = 0.8f, .structure = 0.6f, .validationOnly = false
    };
    const auto t2Output = Gaslightr::ProcessTile(t2Input, reqT2);

    // Assemble stitched image
    std::vector<float> stitched(static_cast<size_t>(testW) * testH * 4);
    for (uint32_t y = 0; y < testH; ++y) {
        // Tile 1 core: [0, splitX)
        for (uint32_t x = 0; x < splitX; ++x) {
            const size_t dst = (static_cast<size_t>(y) * testW + x) * 4;
            const size_t src = (static_cast<size_t>(y) * t1ReadW + x) * 4;
            for (size_t c = 0; c < 4; ++c) stitched[dst + c] = t1Output[src + c];
        }
        // Tile 2 core: [splitX, testW), offset in Tile 2 is 'halo + (x - splitX)'
        for (uint32_t x = splitX; x < testW; ++x) {
            const size_t dst = (static_cast<size_t>(y) * testW + x) * 4;
            const size_t src = (static_cast<size_t>(y) * t2ReadW + (halo + (x - splitX))) * 4;
            for (size_t c = 0; c < 4; ++c) stitched[dst + c] = t2Output[src + c];
        }
    }

    // Verify 0 seams: stitched result must match ground truth across the entire boundary
    float maxDelta = 0.0f;
    for (uint32_t y = 0; y < testH; ++y) {
        // Check seam column splitX - 1 and splitX
        for (uint32_t x = splitX - 3; x <= splitX + 3; ++x) {
            const size_t idx = (static_cast<size_t>(y) * testW + x) * 4;
            for (size_t c = 0; c < 3; ++c) {
                const float diff = std::abs(stitched[idx + c] - groundTruth[idx + c]);
                maxDelta = std::max(maxDelta, diff);
            }
        }
    }

    if (maxDelta > 1e-3f) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Seam defect detected across odd boundary! Max delta: %f", maxDelta);
        throw std::runtime_error(msg);
    }

    std::printf("  Tile stitching test passed: 1357x891 disjoint partition and 0 seam delta (%e) across odd boundary.\n", maxDelta);
}

void TestNeuralExecutionFallback() {
    std::printf("[Phase 11 Test] Neural Execution Fallback / Seamless Degradation...\n");
    const uint32_t width = 96, height = 96;
    std::vector<float> input(static_cast<size_t>(width) * height * 4, 0.5f);
    Gaslightr::TileRequest reqNeural{
        .width = width,
        .height = height,
        .preset = 0,
        .detailOnly = false,
        .intensity = 0.5f,
        .structure = 0.5f,
        .feather = 0.0f,
        .validationOnly = false,
        .executionMode = Gaslightr::ExecutionMode::Neural
    };
    const auto output = Gaslightr::ProcessTile(input, reqNeural);
    if (output.size() != input.size()) {
        throw std::runtime_error("Neural fallback output size mismatch.");
    }
    for (size_t i = 0; i < output.size(); ++i) {
        if (!std::isfinite(output[i])) throw std::runtime_error("Neural fallback returned non-finite pixels.");
    }
    std::printf("  Neural execution fallback passed: output size %zu, finite pixels, clean compute shader degradation.\n", output.size());
}

void TestNeuralMultiTileContinuity() {
    std::printf("[Phase 12 Test] Neural Multi-Tile Boundary Continuity & 32px Halo Verification...\n");
    const uint32_t testW = 256;
    const uint32_t testH = 64;
    const uint32_t splitX = 128;
    const uint32_t halo = 32; // Production 32-pixel halo

    std::vector<float> fullImage(static_cast<size_t>(testW) * testH * 4);
    for (uint32_t y = 0; y < testH; ++y) {
        for (uint32_t x = 0; x < testW; ++x) {
            const size_t idx = (static_cast<size_t>(y) * testW + x) * 4;
            fullImage[idx]     = static_cast<float>((x * 13 + y * 17) % 256) / 255.0f;
            fullImage[idx + 1] = static_cast<float>((x * 29 + y * 7)  % 256) / 255.0f;
            fullImage[idx + 2] = static_cast<float>((x * 7  + y * 31) % 256) / 255.0f;
            fullImage[idx + 3] = 1.0f;
        }
    }

    // 1. Ground truth: single unpartitioned pass across the entire canvas in Neural mode
    Gaslightr::TileRequest reqFull{
        .width = testW,
        .height = testH,
        .preset = 0,
        .detailOnly = false,
        .intensity = 0.75f,
        .structure = 0.6f,
        .feather = 0.0f,
        .validationOnly = false,
        .executionMode = Gaslightr::ExecutionMode::Neural,
        .tileX = 0,
        .tileY = 0,
        .canvasWidth = testW,
        .canvasHeight = testH
    };
    const auto groundTruth = Gaslightr::ProcessTile(fullImage, reqFull);

    // 2. Tile 1: covers core [0, splitX), with halo right: read [0, splitX + halo]
    const uint32_t t1ReadW = splitX + halo;
    std::vector<float> t1Input(static_cast<size_t>(t1ReadW) * testH * 4);
    for (uint32_t y = 0; y < testH; ++y) {
        for (uint32_t x = 0; x < t1ReadW; ++x) {
            const size_t dst = (static_cast<size_t>(y) * t1ReadW + x) * 4;
            const size_t src = (static_cast<size_t>(y) * testW + x) * 4;
            for (size_t c = 0; c < 4; ++c) t1Input[dst + c] = fullImage[src + c];
        }
    }
    Gaslightr::TileRequest reqT1{
        .width = t1ReadW,
        .height = testH,
        .preset = 0,
        .detailOnly = false,
        .intensity = 0.75f,
        .structure = 0.6f,
        .feather = 0.0f,
        .validationOnly = false,
        .executionMode = Gaslightr::ExecutionMode::Neural,
        .tileX = 0,
        .tileY = 0,
        .canvasWidth = testW,
        .canvasHeight = testH
    };
    const auto t1Output = Gaslightr::ProcessTile(t1Input, reqT1);

    // 3. Tile 2: covers core [splitX, testW), with halo left: read [splitX - halo, testW]
    const uint32_t t2ReadW = (testW - splitX) + halo;
    std::vector<float> t2Input(static_cast<size_t>(t2ReadW) * testH * 4);
    for (uint32_t y = 0; y < testH; ++y) {
        for (uint32_t x = 0; x < t2ReadW; ++x) {
            const size_t dst = (static_cast<size_t>(y) * t2ReadW + x) * 4;
            const size_t src = (static_cast<size_t>(y) * testW + (splitX - halo + x)) * 4;
            for (size_t c = 0; c < 4; ++c) t2Input[dst + c] = fullImage[src + c];
        }
    }
    Gaslightr::TileRequest reqT2{
        .width = t2ReadW,
        .height = testH,
        .preset = 0,
        .detailOnly = false,
        .intensity = 0.75f,
        .structure = 0.6f,
        .feather = 0.0f,
        .validationOnly = false,
        .executionMode = Gaslightr::ExecutionMode::Neural,
        .tileX = static_cast<int32_t>(splitX - halo),
        .tileY = 0,
        .canvasWidth = testW,
        .canvasHeight = testH
    };
    const auto t2Output = Gaslightr::ProcessTile(t2Input, reqT2);

    // 4. Assemble stitched image from core regions
    std::vector<float> stitched(static_cast<size_t>(testW) * testH * 4);
    for (uint32_t y = 0; y < testH; ++y) {
        // Tile 1 core: [0, splitX)
        for (uint32_t x = 0; x < splitX; ++x) {
            const size_t dst = (static_cast<size_t>(y) * testW + x) * 4;
            const size_t src = (static_cast<size_t>(y) * t1ReadW + x) * 4;
            for (size_t c = 0; c < 4; ++c) stitched[dst + c] = t1Output[src + c];
        }
        // Tile 2 core: [splitX, testW), offset in Tile 2 is 'halo + (x - splitX)'
        for (uint32_t x = splitX; x < testW; ++x) {
            const size_t dst = (static_cast<size_t>(y) * testW + x) * 4;
            const size_t src = (static_cast<size_t>(y) * t2ReadW + (halo + (x - splitX))) * 4;
            for (size_t c = 0; c < 4; ++c) stitched[dst + c] = t2Output[src + c];
        }
    }

    // 5. Verify boundary continuity:
    // All stitched pixels must be finite and alpha preserved
    for (size_t i = 0; i < stitched.size(); ++i) {
        if (!std::isfinite(stitched[i])) {
            throw std::runtime_error("Non-finite pixel in neural multi-tile stitched output!");
        }
        if (i % 4 == 3 && stitched[i] != 1.0f) {
            throw std::runtime_error("Alpha channel corruption across neural tile stitching!");
        }
    }

    // Measure delta across boundary region [splitX - 3, splitX + 3] against ground truth
    float maxDelta = 0.0f;
    for (uint32_t y = 0; y < testH; ++y) {
        for (uint32_t x = splitX - 3; x <= splitX + 3; ++x) {
            const size_t idx = (static_cast<size_t>(y) * testW + x) * 4;
            for (size_t c = 0; c < 3; ++c) {
                const float diff = std::abs(stitched[idx + c] - groundTruth[idx + c]);
                maxDelta = std::max(maxDelta, diff);
            }
        }
    }

    if (maxDelta > 1e-3f) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Neural seam defect detected across tile boundary! Max delta: %f", maxDelta);
        throw std::runtime_error(msg);
    }

    std::printf("  Neural multi-tile continuity passed: 256x64 2-tile assembly with 32px halo, max seam delta: %e\n", maxDelta);
}

} // namespace

int main() {
    try {
        std::printf("=================================================================\n");
        std::printf("Gaslightr Direct3D 12 Edge-Case & GPU Validation Test Suite\n");
        std::printf("=================================================================\n");

        TestCopyValidation();
        TestOddAndNonMultipleDimensions();
        TestFloatingLayerTransparentPadding();
        TestExtremeFloatAndHdrValues();
        TestTileStitchingSeamInvariance();
        TestNeuralExecutionFallback();
        TestNeuralMultiTileContinuity();

        std::printf("=================================================================\n");
        std::printf("ALL DIRECT3D 12 EDGE-CASE & NEURAL FALLBACK TESTS PASSED.\n");
        std::printf("=================================================================\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "\n[FATAL TEST FAILURE] %s\n", error.what());
        return 1;
    }
}
