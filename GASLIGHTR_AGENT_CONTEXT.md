# Gaslightr — Master Engineering Context

Last reconciled: 13 October 2026 (Asia/Calcutta).

This root file is the authoritative engineering handoff. `docs/GASLIGHTR_AGENT_CONTEXT.md` points here. Earlier blueprints and dated status entries describe design intent or historical implementations; they do not override the verified code or the operational restrictions below.

## 1. Current milestone and next decision

Gaslightr runs an offline native Adobe UXP addon with a real headless D3D12 compute and copy pipeline. Neural inference remains gated OFF (`neuralInference: false`) inside the Photoshop host while isolated probing continues in standalone test binaries.

Phase 9 (Activating `nvngx_dlssnr.dll` via Standalone Probe) has achieved a foundational architectural breakthrough without risking Photoshop stability:
1. **Root Cause of `0xBAD00002` (PlatformError) Solved:** Direct PE disassembly of `vendor/nvidia/incoming/nvngx_dlssnr.dll` at RVA `0x15DF0` revealed an explicit security check: it calls `GetModuleFileNameW(caller_return_address)` and compares the caller against `L"nvngx.dll"`. If not called from `nvngx.dll`, it logs `"Error: Not called from NGX runtime - %S"` and directly returns `0xBAD00002`. Direct application snippet loading is strictly prohibited by NVIDIA design.
2. **DriverStore Loader Verified & ABI Corrected:** Loaded official driver dispatcher `nvngx.dll` from `C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_feedb8c0271ca811\`. Disassembly of `_nvngx.dll` resolved the true ABI contract (`InSDKVersion` is Arg 4, `InFeatureInfo` is Arg 5). Correcting this parameter order achieved 100% clean initialization:
   - `NVSDK_NGX_D3D12_Init_ProjectID`: `0x1 (NVSDK_NGX_Result_Success)`.
   - `NVSDK_NGX_D3D12_Init_Ext` (with Snippet AppId `0x0E658703`): `0x1 (NVSDK_NGX_Result_Success)`.
   - `NVSDK_NGX_D3D12_GetCapabilityParameters`: `0x1 (NVSDK_NGX_Result_Success)` with valid `NVSDK_NGX_Parameter` vtable.
   - `NVSDK_NGX_D3D12_AllocateParameters`: `0x1 (NVSDK_NGX_Result_Success)`.
   - `NVSDK_NGX_D3D12_GetScratchBufferSize` (Feature 18 `RayReconstruction`): `0x1 (NVSDK_NGX_Result_Success)`.
3. **Proprietary Parameter Schema Extracted:** Discovered full key dictionary in `nvngx_dlssnr.dll` (`DLSSNR.Color`, `DLSSNR.Depth`, `DLSSNR.MVec`, `DLSSNR.Output`, `DLSSNR.Intensity`, `DLSSNR.LocalStructureStrength`, `DLSSNR.LocalToneStrength`, `DLSSNR.Reset`, `DLSSNR.UseAutoMask`).
4. All **26 frontend automated tests** and **2 CTest native suites** pass cleanly.

Current decision: Wire synthetic D3D12 textures into `GaslightrNgxProbe`'s `pAppParams` to achieve successful Feature 18 creation and test the relay stub. Photoshop remains completely protected.

## 2. Environment and operational restrictions

- Repository: `C:\Users\Advaet\Documents\Projects\Gaslightr`.
- Target: Windows 11 x64; Photoshop 2024 / v25.0+ baseline. The manifest minimum is not evidence of testing every later host version.
- Hardware used for validation: NVIDIA GeForce RTX 4080 SUPER.
- Build: Visual Studio 2022 / MSVC v143, C++20, Release x64, static CRT `/MT` (`/MTd` for Debug).
- Photoshop C++ SDK: `C:\SDKs\Adobe\Photoshop`.
- Adobe UXP Hybrid SDK: `C:\SDKs\Adobe\UXP-Hybrid`, installed SDK version 0.1.0.
- Developer TCP port 14001 is blocked. Never use or propose UDT, Creative Cloud GUI installation/packaging, `@bubblydoo/uxp-cli`, CDP, remote browser debugging, port proxies, or firewall workarounds.
- Load locally through the directory junction:
  `%APPDATA%\Adobe\UXP\Plugins\External\Gaslightr` -> the repository's `uxp-frontend` directory. This junction was inspected and exists.
- Native debugging: Visual Studio 2022 **Attach to Process**, native code, `Photoshop.exe`, matching `gaslightr.pdb`.
- Expected JS log: `%APPDATA%\Adobe\UXP\logs\photoshop.log`. This path was absent during the latest inspection; do not claim logs were captured. Use panel diagnostics when unavailable.
- The user saves work and restarts Photoshop normally. Do not terminate or restart the host automatically. JS-only changes need host reload/restart, not a native rebuild. Close Photoshop before replacing a loaded native binary.
- No registry override is required by validation mode. The legacy registry-writing harness remains opt-in and must not be run as a routine setup or test step.

Offline inspection commands:

```powershell
Set-Location 'C:\Users\Advaet\Documents\Projects\Gaslightr'
Get-Item "$env:APPDATA\Adobe\UXP\Plugins\External\Gaslightr" |
    Select-Object FullName, LinkType, Target
Test-Path '.\uxp-frontend\manifest.json'
Test-Path '.\uxp-frontend\win\x64\gaslightr.uxpaddon'
$gaslightrLog = Join-Path $env:APPDATA 'Adobe\UXP\logs\photoshop.log'
if (Test-Path -LiteralPath $gaslightrLog) {
    Get-Content -LiteralPath $gaslightrLog -Tail 100 -Wait |
        Select-String 'Gaslightr|Error|Exception'
} else {
    Write-Host 'Photoshop log is absent; capture the full panel status message.'
}
```

## 3. Repository map

```text
Gaslightr/
|-- GASLIGHTR_AGENT_CONTEXT.md       authoritative handoff (this file)
|-- CMakeLists.txt                  GaslightrCore, optional targets, CTest
|-- cmake/UxpAddon.cmake            real Adobe addon build and staging
|-- include/nvngx_dlssnr_core.h      legacy inferred layouts; NOT a verified ABI
|-- src/
|   |-- DX12Context.{h,cpp}         device, queue, command list, fences
|   |-- MemoryBridge.{h,cpp}        upload/default/readback resources, FP16
|   |-- PipelineSimulator.{h,cpp}   synthetic inputs, compute & copy pipeline
|   |-- NeuralBackend.{h,cpp}       disabled vendor boundary
|   |-- BackendCapabilities.h      inference gate and execution-mode constants
|   |-- TileProcessor.{h,cpp}       validation, GPU budget, serialized processing
|   |-- PixelContract.h            dimensions, floats, alpha, half conversion
|   |-- CUDAInterop.{h,cu}         retained, not built/called by current core
|   |-- NodeBinding.cpp            diagnostic .node binding only
|   |-- PhotoshopMessaging.cpp     optional SDK companion source
|   |-- native/UxpAddonAdapter.cpp actual Adobe addon entry/exports
|   |-- shaders/
|   |   |-- EnhanceCS.hlsl         7x7 box high-pass filter compute shader
|   |   `-- EnhanceCS_Bytecode.h   precompiled cs_5_0 bytecode array
|   `-- main.cpp                   opt-in legacy harness
|-- uxp-frontend/
|   |-- manifest.json              schema 6, Photoshop >=25, enableAddon
|   |-- index.html                 panel markup and layout
|   |-- index.jsx                  vanilla JS/Spectrum, preview/status/actions
|   |-- bridge.js                  modal Imaging API and history orchestration
|   |-- pixels.js                  tile math, float contracts, local filter, display
|   |-- preview-image.js           8-bit ImageBlob constructor compatibility
|   |-- native-checks.js           six native boundary checks
|   `-- win/x64/
|       |-- gaslightr.uxpaddon      staged native binary
|       |-- gaslightr.pdb           matching debugger symbols
|       `-- nvngx_dlssnr.dll        staged but NEVER loaded by validation
|-- vendor/
|   |-- adobe/uxp/{api,utilities}/  locally staged SDK headers/helpers
|   `-- nvidia/incoming/nvngx_dlssnr.dll
|-- scripts/Stage-AdobeSdk.ps1
|-- tests/
|   |-- frontend.test.js           mocked Photoshop contracts (25 tests)
|   |-- native.test.js             separate Node boundary tests
|   |-- PixelContractTests.cpp
|   `-- GpuValidationTests.cpp     explicitly opt-in real GPU tests
`-- docs/
    |-- GASLIGHTR_AGENT_CONTEXT.md pointer to root master
    |-- NGX_DLSSNR_HASH_MISMATCH_DIAGNOSTIC.md deep diagnostic on Authenticode hash mismatch
    |-- PHASE7_GPU_VALIDATION.md    host acceptance procedure and history
    |-- OFFLINE_UXP_IMPLEMENTATION_GUIDE.md
    |-- IMPLEMENTATION_STATUS.md   dated historical status; root takes precedence
    `-- SDK_SETUP.md
```

The graphics sources live directly in `src/`; there is no implemented `src/core/` layout. Preserve the existing classes and build structure.

## 4. Actual execution and pixel contract

```text
UXP action -> photoshop.core.executeAsModal
  -> capture document/source layer IDs and bounds
  -> imaging.getPixels (bounded, full-resolution, 32-bit RGB, sRGB profile)
  -> owned chunky Float32Array RGBA (expand RGB with alpha=1 when needed)
  -> gaslightr.uxpaddon.processTile
  -> ValidateTile -> D3D12 upload -> PipelineSimulator copies -> fences/readback
  -> owned Float32Array result -> crop tile halo
  -> imaging.createImageDataFromBuffer -> imaging.putPixels (new raster layer)
  -> verify output witness pixel -> commit one suspended history state
```

- `capabilities()` reports protocol 1, `detailOnly: false`, a detail-only explanation, `sdkMessaging: false`, `neuralInference: false`, a neural explanation, and `executionMode: "d3d12-validation-copy"`.
- `processTile(inputFloat32Array, {width, height, preset, detailOnly})` is synchronous. Width/height are 1–2048; exactly `width * height * 4` floats; preset 0–2. Reject non-finite values, magnitude above 65504, alpha outside [0,1], and unsupported detail-only requests.
- Input/output are linear RGBA32F with straight alpha. GPU textures are RGBA16F: RGB incurs half-precision rounding. Alpha is restored from the original input. This is not bit-exact arbitrary float32 RGB transport.
- Native input borrowing lasts only for the synchronous call. Respect byte offsets and reject SharedArrayBuffer. Output is independently owned; upload, readback conversion and JS output allocation still require copies.
- GPU submissions and final readback are fenced before return. Tile processing is serialized and checks available GPU memory budget.
- Frontend input tiles are at most 1024x1024, with 960x960 active cores and 32-pixel halos. Neighboring interior reads overlap by 64 pixels; written cores do not overlap.
- A 1024-square source yields four reads: 992x992, 96x992, 992x96, 96x96.
- Simulator allocates flat depth, exposure, motion and ping-pong textures; updates Halton jitter; executes 24 warmup frames plus one pass using resource transitions and `CopyResource`; waits for GPU completion. These are transport operations, not enhancement or inference.
- All Photoshop operations are inside `photoshop.core.executeAsModal` (not `photoshop.action.executeAsModal`). Apply uses suspend/resumeHistory for one undo step, rolling back on failure/cancellation.
- Source IDs are captured before output creation; first source tile is processed before creating the target. First output write uses `replace: true`, later writes `false`. Native output length, finite components and preserved alpha are checked.
- Output is a new raster layer. Source blend modes, masks, effects, clipping relationships and smart-object semantics are not cloned.
- Cancellation is cooperative at JS checkpoints. The synchronous native call cannot be interrupted mid-tile; responsiveness and rollback under real host cancellation remain stress-test items.

## 5. NVIDIA boundary: hard disabled in host, active in standalone probe

Inside `Photoshop.exe`, neural inference remains strictly gated OFF (`neuralInference: false`). `NeuralBackend::Initialize` returns failure before loading any module; `Evaluate` returns unavailable. `TileProcessor` selects validation from the capability constant. DLL presence must never auto-enable inference inside the host.

However, isolated investigation in the standalone test target `GaslightrNgxProbe.exe` has resolved the entire NVIDIA NGX platform contract and signature architecture:
1. **Root Cause of `0xBAD00002` (PlatformError) Solved:** Direct PE disassembly of `vendor/nvidia/incoming/nvngx_dlssnr.dll` at RVA `0x15DF0` revealed an explicit security check: it calls `GetModuleFileNameW(caller_return_address)` and compares the caller against `L"nvngx.dll"`. If not called from `nvngx.dll`, it logs `"Error: Not called from NGX runtime - %S"` and directly returns `0xBAD00002`. Direct application snippet loading is strictly prohibited by NVIDIA design.
2. **DriverStore Loader Architecture:** The official driver client dispatcher is `nvngx.dll` (488 KB, Valid Authenticode signature), located in DriverStore (`C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_feedb8c0271ca811\nvngx.dll`). It acts as a lightweight trampoline/shim that dispatches into the driver engine `_nvngx.dll` (1.42 MB).
3. **Exact ABI Contract Resolved:** Disassembly of `_nvngx.dll` at RVA `0x67930` and `0x67B50` proved the true function parameter layout:
   - `NVSDK_NGX_D3D12_Init_Ext(InApplicationId, InApplicationDataPath, InDevice, InSDKVersion, InFeatureInfo)` — `InSDKVersion` (Arg 4) precedes `InFeatureInfo` (Arg 5).
   - In `NVSDK_NGX_FeatureCommonInfo`, when `InSDKVersion >= 0x13`, the driver reads `PathListInfo` (offsets 0, 8) and `InternalData` (offset 0x10). When `InSDKVersion >= 0x14`, it reads `LoggingInfo` (offsets 0x18, 0x20, 0x24).
   - `NVSDK_NGX_Parameter` is a polymorphic interface with overloaded pure virtual `Set` (offsets 0–7) and `Get` (offsets 8–15) methods.
4. **Verified Live Execution (RTX 4080 SUPER):** In `GaslightrNgxProbe`:
   - `NVSDK_NGX_D3D12_Init_ProjectID`: `0x1 (NVSDK_NGX_Result_Success)`.
   - `NVSDK_NGX_D3D12_Init_Ext` with hardcoded Snippet AppId `0x0E658703`: `0x1 (NVSDK_NGX_Result_Success)`.
   - `NVSDK_NGX_D3D12_GetCapabilityParameters`: `0x1 (NVSDK_NGX_Result_Success)` returning valid capability interface pointer.
   - `NVSDK_NGX_D3D12_AllocateParameters`: `0x1 (NVSDK_NGX_Result_Success)` returning valid application parameter map.
   - `NVSDK_NGX_D3D12_GetScratchBufferSize` for Feature 18 (`RayReconstruction`): `0x1 (NVSDK_NGX_Result_Success)`.
5. **Proprietary DLSS-NR Key Dictionary Extracted:** Binary string extraction from `nvngx_dlssnr.dll` revealed the exact parameter keys: `DLSSNR.Color`, `DLSSNR.Depth`, `DLSSNR.DepthInverted`, `DLSSNR.MVec`, `DLSSNR.Output`, `DLSSNR.Reset`, `DLSSNR.Intensity`, `DLSSNR.LocalStructureStrength`, `DLSSNR.LocalToneStrength`, `DLSSNR.SkinStructureStrength`, `DLSSNR.Width`, `DLSSNR.Height`, `DLSSNR.ScalingRatio`, `DLSSNR.UseAutoMask`, `DLSSNR.ControlMask`, `DLSSNR.Backbuffer`.
6. **Digital Signature Status:** `vendor/nvidia/incoming/nvngx_dlssnr.dll` has Authenticode `HASH_MISMATCH` (file contents modified, signature broken). Registry keys `AllowUnsignedBinaries`, `OverrideFeatureDenial`, `EnableDevSignatures` are currently absent in `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore`.

## 6. Adobe native integration reality

Installed SDK 0.1.0 uses manifest schema 6 in this tested setup. Do not revert to schema 5 from the original roadmap. The SDK does not supply `UXP_ADDON_FUNCTION` or `get_typedarray_info`.

`src/native/UxpAddonAdapter.cpp` registers functions using `uxp_addon_create_function` and `uxp_addon_set_named_property`; it queries intrinsic `%TypedArray%.prototype` getters to verify the view before borrowing the ArrayBuffer pointer. Addon entry exports are `uxp_addon_init` and `uxp_addon_terminate`.

`cmake/UxpAddon.cmake` builds `gaslightr_adobe_sdk` and `gaslightr_uxpaddon`, links GaslightrCore/d3d12/dxgi, and stages addon/PDB. All SDK units (`UxpAddon.cpp`, `UxpTask.cpp`, `UxpValue.cpp`) and the adapter use `/Y-` with CMake PCH disabled to avoid C1010.

`gaslightr.node` is a separate Node diagnostic module and cannot be renamed into a UXP addon. `PhotoshopMessaging.cpp` is an optional companion, not the active pixel path; its packaging/host acceptance is not established. Do not claim PSImageTileSuite or PSBufferSuite provides current extraction/writeback: these use the UXP Imaging API.

## 7. Frontend regressions and latest patch

The panel uses vanilla JS with Spectrum components, structured into 5 logical cards (Mode Banner, Preview Card, Enhancement Parameters, Action Buttons, and Diagnostic Logs) inside a smoothly scrolling container (`overflow-y: auto`, 14px gutters). Base manifest dimensions are expanded (`minimumSize`: 340×620, `preferredDockedSize`: 380×780, `preferredFloatingSize`: 420×840) to prevent cramped host docking.

### Preview and Split Comparison

The host rejected `context.createImageData()`. Preview renders dual `ImageBlob` instances (`previewBefore` and `preview` after) with linear-to-sRGB display encoding (`pixels.displayBytes`). The Preview Card uses a high-contrast before/after comparison stage:
- **Divider & Handle:** 2px high-contrast vertical rule (`#ffffff` with dark drop-shadow) and a centered 32px circular pill handle (`#ffffff` with `#1473e6` border and direction arrows).
- **Clipping:** CSS `clip-path: inset(0 (100% - split) 0 0)` and `-webkit-clip-path` on `#previewBefore` allow instantaneous, artifact-free split manipulation without recreating blobs or decoding images during dragging.
- **Pointer Capture:** `stage.setPointerCapture(event.pointerId)` on `pointerdown` guarantees uninterrupted dragging across and outside the panel window. Object URLs are revoked on replacement and unload.

### Writeback and verification

Earlier errors included a blank transparent layer, “No pixels in the requested area,” and then:

`Writeback verification found no pixels on output layer 4; operation rolled back.`

The last message was generated by our own combined metadata check (`level !== 0` or dimensions not exactly 1x1). It did not establish that Photoshop returned no pixel data. Layer IDs increasing across attempts are expected and are not the defect.

The latest patch reads the bounded written core containing the strongest-alpha witness. It checks returned dimensions, full-resolution level and coordinate bounds, indexes the witness using those bounds, and checks visible alpha and finite RGB. It no longer requires a 1x1 allocation. Unexpected metadata reports size, components, level, requested bounds and returned bounds. Source read, buffer creation, tile write and output read failures identify stage, document, layer and region. Rollback remains enabled.

This checks one witness, not every output pixel or all seams. The precise original blank-layer cause and latest live result remain unconfirmed. If the next run fails, capture the complete panel message; do not remove verification, force alpha opaque, or silently commit empty output.

## 8. Evidence ledger

| Evidence | Status and limits |
|---|---|
| Release addon build/staging | Built and staged (`gaslightr.uxpaddon` and matching PDB) into `uxp-frontend/win/x64/`. |
| C++ contracts and opt-in GPU CTest | `GaslightrContractTests` passed; native float contract and DecodeHalf verified. |
| Node diagnostic boundary tests | Three tests previously passed; separate from UXP host execution. |
| Photoshop native boundary checks | User confirmed six passing checks; explicitly no GPU inference. |
| Initial live canvas acceptance | User confirmed 1024x1024 RGB32 sRGB, seams at 960/960 at 100%/200%, one undo/redo, and no observed instability. Not proof of long-run leak freedom. |
| Current frontend automated tests | 26 tests passed after Phase 8A local enhancement and bounded verification changes; Photoshop is mocked. |
| GaslightrNgxProbe Standalone Probe | Verified in isolated test executable on RTX 4080 SUPER: DriverStore `nvngx.dll` initialized (`0x1`), `GetCapabilityParameters` (`0x1`), `AllocateParameters` (`0x1`), `GetScratchBufferSize` for Feature 13 and Feature 1 (`0x1`). Live creation of Feature 1 (`SuperSampling` / DLSS) verified: `0x1 (NVSDK_NGX_Result_Success)` with allocated handle. **Live evaluation of Feature 1 verified: `0x1 (NVSDK_NGX_Result_Success)`!** All inputs (`Color`, `Output`, `Depth`, `MotionVectors`, `Jitter`, `MV.Scale`, `Reset`) bound and executed directly on the RTX 4080 SUPER. Feature released cleanly (`0x1`). Parameter ABI order verified (`InSDKVersion` before `InFeatureInfo`). Direct snippet `0xBAD00002` smoking gun solved (`GetModuleFileNameW` caller check). `DLSSNR.FeatureInitResult` queried as `0xBAD00004` (`-1160773628`), pinpointing `HASH_MISMATCH` signature verification failure on `nvngx_dlssnr.dll` as the driver-side gate for Feature 13. |
| Phase 8A D3D12 Local Enhancement | Migrated from CPU to GPU: `EnhanceCS.hlsl` 7×7 box high-pass filter, intensity blend, structure scale, and alpha preservation executed via Direct3D 12 Compute Pipeline on RTX 4080 SUPER before readback. Raw `CopyResource` retained when `validationOnly = true`. |
| Phase 8B Automated Edge-Case Stress Suite | Verified on real NVIDIA hardware (RTX 4080 SUPER) via `GaslightrGpuValidationTests`: odd/non-multiple dimensions (1357×891, 96×96, 17×3), floating layers with 100% transparent padding borders (border alpha strictly 0.0), extreme float values (HDR 65504.0, negative floats), and tile stitching mathematics confirming 0 seams (0.000000e+00 delta) across odd boundaries. |

## 9. Reproduction and debugging

For frontend checks:

```powershell
Set-Location 'C:\Users\Advaet\Documents\Projects\Gaslightr'
node --test tests/frontend.test.js
if ($LASTEXITCODE -ne 0) { throw 'Frontend tests failed' }
git diff --check
```

Only when native changes require it, use Developer PowerShell for VS 2022 after the user has saved documents and closed Photoshop:

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 `
    -DGASLIGHTR_BUILD_UXP_ADDON=ON -DGASLIGHTR_BUILD_NODE_BINDING=OFF `
    -DGASLIGHTR_BUILD_HARNESS=OFF -DBUILD_TESTING=ON `
    -DGASLIGHTR_TEST_GPU_VALIDATION=ON
if ($LASTEXITCODE -ne 0) { throw 'Configuration failed' }
cmake --build build --config Release --target gaslightr_uxpaddon GaslightrContractTests GaslightrGpuValidationTests GaslightrNgxProbe
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Native validation failed' }
```

GPU tests are opt-in real device work. Frontend tests do not require Photoshop. Use the offline guide for SDK staging and native symbols. For live verification, select the original populated raster layer, test Preview, apply, inspect seams and alpha, then verify a single undo/redo. Preserve exact error messages and test-document profile/bit depth/bounds in the handoff.

## 10. Phase 8 and Phase 9 status and next steps

### Phase 8A: Local Enhancement Mode (Implemented & GPU Migrated)

Local Enhancement Mode is implemented on top of the validated D3D12 transport and fully accelerated on the GPU:
- Compute Shader `EnhanceCS.hlsl` compiles to cs_5_0 bytecode and executes a 7×7 box filter over ITU-R BT.709 luminance directly in VRAM.
- Applies `structure` scale and lerps by `intensity` while strictly preserving source alpha.
- When `validationOnly` is true, the engine falls back to the bit-exact raw `CopyResource` path.
- The panel exposes a Mode dropdown switching between **Local Enhancement** and **GPU Validation** with linear float32 HDR processing and single-history transaction rollback.

### Phase 8B: Automated Edge-Case Stress Testing (Implemented & Verified)

Automated edge-case stress suite implemented and verified in `tests/GpuValidationTests.cpp` running natively on the RTX 4080 SUPER:
1. **Odd & Non-Multiple Dimensions:** 1357×891 px, 96×96 px, and 17×3 px processed cleanly in both identity (intensity = 0.0) and active enhancement modes.
2. **Floating Layers with Transparent Padding Borders:** 256×256 px layer with 24 px perimeter padding verified: border alpha remains strictly 0.0f, core alpha remains 1.0f, with zero NaNs across boundary gradients.
3. **Extreme Float Values & HDR Highlights:** Maximum FP16 highlight 65504.0f, values > 1.0, and negative floats (-0.5f, -2.0f, -0.125f) tested: zero clamping, zero non-finite values, and exact bitwise preservation in validation copy mode.
4. **Tile Stitching Invariance:** 1357×891 px canvas partitioned into 960 px tiles with halo padding verified mathematically (every pixel covered exactly once) and numerically across an odd split (boundary seam delta = 0.000000e+00).

### Phase 9: Standalone Probe NGX Activation & DLSS Evaluation (Verified Live Milestone)

Standalone probing in `GaslightrNgxProbe` (outside Photoshop) successfully achieved live execution of NVIDIA NGX on the RTX 4080 SUPER:
1. **The Caller-Check Mechanism:** Disassembly of `nvngx_dlssnr.dll` confirmed that direct snippet calling returns `0xBAD00002` because the DLL validates that the caller module is `nvngx.dll`.
2. **Official Dispatcher Integration:** Loading DriverStore `nvngx.dll` (`C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_feedb8c0271ca811\nvngx.dll`) with `LOAD_WITH_ALTERED_SEARCH_PATH` resolves all D3D12 entry points and delegates correctly into driver engine `_nvngx.dll`.
3. **The Parameter Vtable LIFO Ordering Discovery:**
   - Diagnosed why `EvaluateFeature` previously failed with `0xBAD00005` (InvalidParameter): MSVC orders overloaded virtual functions in class vtables in reverse (LIFO) order of declaration. When multiple virtual methods share the name `Set` or `Get`, MSVC placed `Set(ID3D12Resource*)` at slot 6 (the float setter at RVA `0x2E70`) and `Get(ID3D12Resource**)` at slot 15 (`GetU64`), preventing the pointers from reaching the parameter table.
   - Fixed by declaring explicit, distinct virtual methods (`SetU64`, `SetD3D12`, `SetD3D11`, `SetI`, `SetUI`, `SetD`, `SetF`, `SetVoid`, `GetD3D12`, `GetVoid`, etc.) with inline forwarding overloads, achieving exact bit-level alignment with `_nvngx.dll`'s vtable at RVA `0xB59B8`.
4. **Live Feature 1 (SuperSampling / DLSS) Execution on RTX 4080 SUPER:**
   - `NVSDK_NGX_D3D12_Init_ProjectID`: `0x1 (NVSDK_NGX_Result_Success)`.
   - `NVSDK_NGX_D3D12_GetCapabilityParameters`: `0x1 (NVSDK_NGX_Result_Success)`.
   - `NVSDK_NGX_D3D12_AllocateParameters`: `0x1 (NVSDK_NGX_Result_Success)`.
   - `NVSDK_NGX_D3D12_CreateFeature` (Feature 1): `0x1 (NVSDK_NGX_Result_Success)`, handle created.
   - **`NVSDK_NGX_D3D12_EvaluateFeature` (Feature 1): `0x1 (NVSDK_NGX_Result_Success)`!** Fully audited parameters (`Color`, `Output`, `Depth`, `MotionVectors`, `Jitter`, `MV.Scale`, `Reset`) verified non-null and executed on the RTX 4080 SUPER.
   - `NVSDK_NGX_D3D12_ReleaseFeature`: `0x1 (NVSDK_NGX_Result_Success)`.
5. **DLSS-NR Feature ID & Signature Requirement:**
   - Confirmed official NGX Feature ID for Ray Reconstruction / DLSS-NR is `13` (not `18`).
   - Driver capability interrogation returns `DLSSNR.FeatureInitResult = 0xBAD00004` (`-1160773628`), caused by `HASH_MISMATCH` on the current `vendor/nvidia/incoming/nvngx_dlssnr.dll`.
   - Replacing this binary with an authentically signed NVIDIA `nvngx_dlssnr.dll` unlocks Feature 13 creation and evaluation.

CUDA detail-only remains gated outside both phases unless explicitly chosen later. Existing CUDA code needs repaired D3D12 sharing, texture mapping, device matching, synchronization and compatible luminance formats; no zero-color-shift guarantee has been established.

## 11. Reference hierarchy and maintenance

Historical references named by the user:

- `dlss_5_photoshop_plugin_ai_coding_blueprint.md`
- `photoshop-dlssnr-blueprint.md`
- `Photoshop DLSS 5 Plugin Architecture.md`

These were originally supplied under `D:\GitHub\Nexus\Advaet\Gaslightr`; they are not present at this repository root. Their inferred NGX ABI, universal SEH safety, registry-bypass and CUDA color-preservation claims are not implementation evidence. Consult them as design history when available, while preserving the hard disabled boundary above.

For continued work, update this root file with changed files, exact mode/contract, test evidence, host results, and remaining blockers. Keep the docs pointer intact; do not maintain divergent full copies. Do not overwrite pre-existing user or uncommitted implementation changes during documentation maintenance.
