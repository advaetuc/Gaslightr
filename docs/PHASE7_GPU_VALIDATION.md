# Phase 7: Photoshop acceptance with D3D12 validation copies

## Current revision status — 13 October 2026

The initial success below predates subsequent preview/writeback regressions. The current frontend has 25 passing mocked-host tests and awaits a fresh Photoshop run. Preview uses an owned 8-bit buffer with a narrowly scoped ImageBlob constructor compatibility retry. Output verification now reads a bounded written core and maps its witness pixel against returned bounds, replacing the misleading 1x1 check. Unexpected metadata includes actual dimensions, resolution level and coordinates; failures still roll back. See the [master context](../GASLIGHTR_AGENT_CONTEXT.md) for the complete current contract and Phase 8A/8B decision. Historical test counts below refer to their original runs.

## Earlier live acceptance and procedure

Implemented and GPU-tested on 13 October 2026. The user subsequently confirmed live acceptance in Photoshop 2024 on the RTX 4080 SUPER: a 1024x1024 32-bit RGB sRGB canvas processed successfully, all four tiles joined without visible defects at x=960/y=960 at 100% and 200%, and one undo/redo removed/restored the complete validation layer. The user reported no observed instability or leaks during that run; sustained memory usage and broader edge cases remain to be measured.

Preview exposed an unsupported `context.createImageData` call. The frontend now displays the split RGBA preview through Adobe's documented `ImageBlob` and an HTML image element, releasing replaced object URLs. This preview fix still needs confirmation inside Photoshop. Preview bytes remain an 8-bit display conversion; document pixels continue through the existing float pipeline.

Recommended next gate: confirm the preview, then test 8/16/32-bit sources, non-sRGB profiles, transparent/clipped margins, odd/tiny/negative-offset bounds, deferred cancellation with complete rollback, and repeated runs/unload memory behavior. Keep copy-only validation until these pass. Then add an explicitly named local-enhancement mode while retaining copy validation as a diagnostic option.

## What this build does

`NeuralBackend::Initialize` fails before loading any DLL. `Evaluate` reports inference unavailable. `TileProcessor` selects `SimulatorConfig::validationOnly` because the build has no verified neural backend. DLL presence never enables inference.

The simulator still allocates depth, motion, exposure and ping-pong textures, updates Halton motion vectors, runs 24 warmup frames and one subsequent pass, and waits for the GPU every frame. Its new `EvaluateFrame` records actual D3D12 `CopyResource` operations with resource-state transitions instead of calling the neural backend. Existing upload, readback, finite-value validation, source-alpha preservation and final fence checks remain active. This exercises GPU transfers and queue execution; it does not execute an image-enhancement shader or a neural network.

Capabilities now include:

```json
{
  "protocol": 1,
  "detailOnly": false,
  "sdkMessaging": false,
  "neuralInference": false,
  "neuralInferenceReason": "DLSS-NR ABI is unverified; matching runtime headers and integration contract are unavailable.",
  "executionMode": "d3d12-validation-copy"
}
```

The panel displays GPU validation mode, disables enhancement controls and names its action **Create validation layer**. CPU enhancement blending is bypassed so it cannot conceal faulty GPU output. The validation layer and history state are explicitly labeled.

**Precision:** JS input/output is linear RGBA32F, but MemoryBridge and the simulator use RGBA16F GPU textures. RGB is therefore quantized to half precision. Alpha is restored from the original Float32Array. A 32-bit Photoshop document is supported, but arbitrary RGB values are not expected to survive this path bit-for-bit. Values such as -0.25, 0.125, 1, 4 and 65504 are exactly representable in FP16; 0.1 is not. No HDR clipping to [0,1] is expected.

The runtime is retained in both `vendor/nvidia/incoming/nvngx_dlssnr.dll` and `uxp-frontend/win/x64/nvngx_dlssnr.dll`, with matching SHA-256 hashes. The validation path does not open it. No registry override is needed.

## Source changes

- `src/BackendCapabilities.h`: single source for the unavailable inference capability and processing mode.
- `src/NeuralBackend.h` and `.cpp`: disabled vendor boundary; no LoadLibrary or inferred function calls.
- `src/PipelineSimulator.h` and `.cpp`: explicit validation mode and GPU copy recording within the existing frame loop.
- `src/TileProcessor.cpp`: selects validation before runtime checks; keeps upload, readback and synchronization.
- `src/native/UxpAddonAdapter.cpp` and `src/NodeBinding.cpp`: expose the same truthful capability fields.
- `uxp-frontend/bridge.js`: bypasses CPU enhancement blending and labels validation layer/history.
- `uxp-frontend/index.html`, `index.jsx` and `native-checks.js`: visible mode and updated capability checks.

Do not enable inference by changing only the capability constant. A future verified backend must supply and validate its complete runtime contract first.

## Build again when needed

The current Release binary and matching PDB have already been built and staged. To reproduce from Developer PowerShell for VS 2022, save documents and exit Photoshop normally first:

```powershell
Set-Location 'C:\Users\Advaet\Documents\Projects\Gaslightr'
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 `
    -DGASLIGHTR_BUILD_UXP_ADDON=ON `
    -DGASLIGHTR_BUILD_NODE_BINDING=OFF `
    -DGASLIGHTR_BUILD_HARNESS=OFF `
    -DBUILD_TESTING=ON -DGASLIGHTR_TEST_GPU_VALIDATION=ON
if ($LASTEXITCODE -ne 0) { throw 'Configuration failed' }
cmake --build build --config Release --target gaslightr_uxpaddon GaslightrContractTests GaslightrGpuValidationTests
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Validation failed' }
```

The GPU test is explicitly opt-in and runs real D3D12 work. It covers 17x3, 992x992, 96x992, 992x96 and 96x96 inputs, checks exact output for FP16-representable RGB and original alpha, and verifies the neural DLL is not loaded in that process. Both CTests passed locally. The 15 frontend tests and 3 compiled Node-boundary tests also passed, including history rollback and validation-mode blending bypass. These results do not establish Photoshop-specific color conversion or live history behavior.

## First live canvas pass

1. Start/restart Photoshop and open Gaslightr. Verify the top banner says **GPU validation** and the action says **Create validation layer**. If the old UI appears, fully exit and reopen the host.
2. Click **Check native bridge**. Expect the six boundary checks to pass. These remain separate from the real GPU processing test.
3. Create a disposable **1024x1024 RGB, 32 bits/channel** document with an sRGB profile. Select one normal raster layer covering the canvas. Use an opaque gradient with fine diagonal lines and colored texture across x=960 and y=960.
4. Click **Preview**. Expect a center crop that looks almost unchanged. Minor FP16 quantization can exist. No new layer should be created.
5. Click **Create validation layer**. Expect one new layer named `<source> · Gaslightr GPU validation`, with the same image content and bounds. Expect no neural enhancement. The source remains intact.
6. Inspect x=960, y=960 and their intersection at 100% and 200%. Each input halo extends 32 pixels; adjacent reads overlap by 64 pixels while written cores do not overlap. Look for gaps, shifted rows, dark borders and duplicated pixels. A 1024-square canvas processes four read regions: 992x992, 96x992, 992x96 and 96x96.
7. Open History. There should be one **Gaslightr GPU validation** state. Press **Ctrl+Z** once: the entire output layer must disappear. Redo should restore it as one operation.
8. On a second document, test transparency and HDR highlights. Verify alpha edges and document profile/bit depth. Expect half-precision RGB rounding, not clipping to SDR.
9. For cancellation, use a larger disposable canvas and click Cancel while processing. Cancellation is checked between tiles; partial output must roll back. If processing finishes before the click is handled, that is a completed run, not a cancellation test.

On a native error, read the message at the top of the panel; failure must not leave a partial output layer. Use the offline guide's Visual Studio Attach to Process procedure for diagnosis. Successful validation establishes the Photoshop/D3D12 transport and transaction path, not neural quality or compatibility.
