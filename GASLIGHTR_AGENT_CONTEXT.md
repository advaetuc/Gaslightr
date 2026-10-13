# Gaslightr — Master Engineering Context

Last reconciled: 13 October 2026 (Asia/Calcutta).

This root file is the authoritative engineering handoff. `docs/GASLIGHTR_AGENT_CONTEXT.md` points here. Earlier blueprints and dated status entries describe design intent or historical implementations; they do not override the verified code or the operational restrictions below.

## 1. Current milestone and next decision

Gaslightr runs an offline native Adobe UXP addon with a real headless D3D12 **GPU validation copy** pipeline. It does not currently perform neural inference or local enhancement in the active mode.

The user previously confirmed a successful Photoshop canvas run, seam inspection, and single-step undo/redo. Subsequent preview and writeback regressions reopened acceptance for the current frontend revision. The latest output-verification patch has **25 passing frontend tests**, but has **not yet been confirmed in Photoshop**. Do not equate mock tests or the earlier live run with acceptance of the latest revision.

Current instruction: maintain this handoff and await the user's choice of Phase 8A or Phase 8B. Neither phase has been started by this documentation update. Before declaring either phase accepted, reconfirm preview, populated output, and rollback in the current frontend.

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
|   |-- PipelineSimulator.{h,cpp}   synthetic inputs, temporal copy loop
|   |-- NeuralBackend.{h,cpp}       disabled vendor boundary
|   |-- BackendCapabilities.h      inference gate and execution-mode constants
|   |-- TileProcessor.{h,cpp}       validation, GPU budget, serialized processing
|   |-- PixelContract.h            dimensions, floats, alpha, half conversion
|   |-- CUDAInterop.{h,cu}         retained, not built/called by current core
|   |-- NodeBinding.cpp            diagnostic .node binding only
|   |-- PhotoshopMessaging.cpp     optional SDK companion source
|   |-- native/UxpAddonAdapter.cpp actual Adobe addon entry/exports
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

## 5. NVIDIA boundary: hard disabled

User-supplied export inspection identifies the loose `nvngx_dlssnr.dll` as version 310.8.0.0, x64, with 55 exports. It is an internal NGX feature/snippet binary supplied without matching private headers or samples. Earlier inferred InitializeNR/EvaluateNR names are absent. Internal exports include D3D12 Init_Ext, CreateFeature and EvaluateFeature, but their names do not establish a supported application ABI.

Feature identifiers (including the reported reserved public feature 18), parameter allocation, parameter keys, lifecycle and teardown contracts are unavailable. The user reported an access violation (0xC0000005) when attempting unsupported direct integration. Never guess structs or call those exports to probe compatibility inside Photoshop. SEH cannot make a wrong ABI safe.

`NeuralBackend::Initialize` returns failure before loading any module; `Evaluate` returns unavailable. `TileProcessor` selects validation from the capability constant. DLL presence must never auto-enable inference. Keep both staged DLL copies for future documented integration; do not load, redistribute, or replace them as part of frontend work.

The old header remains for legacy/internal compilation, not as an authoritative NVIDIA definition. Do not enable inference by flipping one flag. A future implementation needs the complete verified runtime contract, hardware requirements, resource semantics and teardown, with isolated validation before host integration.

## 6. Adobe native integration reality

Installed SDK 0.1.0 uses manifest schema 6 in this tested setup. Do not revert to schema 5 from the original roadmap. The SDK does not supply `UXP_ADDON_FUNCTION` or `get_typedarray_info`.

`src/native/UxpAddonAdapter.cpp` registers functions using `uxp_addon_create_function` and `uxp_addon_set_named_property`; it queries intrinsic `%TypedArray%.prototype` getters to verify the view before borrowing the ArrayBuffer pointer. Addon entry exports are `uxp_addon_init` and `uxp_addon_terminate`.

`cmake/UxpAddon.cmake` builds `gaslightr_adobe_sdk` and `gaslightr_uxpaddon`, links GaslightrCore/d3d12/dxgi, and stages addon/PDB. All SDK units (`UxpAddon.cpp`, `UxpTask.cpp`, `UxpValue.cpp`) and the adapter use `/Y-` with CMake PCH disabled to avoid C1010.

`gaslightr.node` is a separate Node diagnostic module and cannot be renamed into a UXP addon. `PhotoshopMessaging.cpp` is an optional companion, not the active pixel path; its packaging/host acceptance is not established. Do not claim PSImageTileSuite or PSBufferSuite provides current extraction/writeback: these use the UXP Imaging API.

## 7. Frontend regressions and latest patch

The panel uses vanilla JS with Spectrum components, a scrollable body and visible status area. Current mode displays **GPU validation** and **Create validation layer**; enhancement controls are unavailable and CPU blending is bypassed.

### Preview

The host rejected `context.createImageData()`. Preview now renders through an image element and ImageBlob, not canvas ImageData. `pixels.displayBytes` converts linear RGB to sRGB display encoding, clamps/rounds to 0–255, and quantizes alpha separately without RGB gamma. Document data remains float32.

`preview-image.js` copies into an exact-size owned Uint8Array and first passes `uint8.buffer` with `componentSize: 8`. The repository also retries a Uint8Array view **only** for the observed “Invalid type of arrayBuffer argument” error, reflecting the API table/example discrepancy encountered during bring-up. Do not simplify this to an assertion that every host universally rejects typed arrays. Only the 8-bit preview path is implemented here; broader constructor support has not been host-validated. Object URLs are revoked on replacement/clear/unload and failed assignment.

### Writeback and verification

Earlier errors included a blank transparent layer, “No pixels in the requested area,” and then:

`Writeback verification found no pixels on output layer 4; operation rolled back.`

The last message was generated by our own combined metadata check (`level !== 0` or dimensions not exactly 1x1). It did not establish that Photoshop returned no pixel data. Layer IDs increasing across attempts are expected and are not the defect.

The latest patch reads the bounded written core containing the strongest-alpha witness. It checks returned dimensions, full-resolution level and coordinate bounds, indexes the witness using those bounds, and checks visible alpha and finite RGB. It no longer requires a 1x1 allocation. Unexpected metadata reports size, components, level, requested bounds and returned bounds. Source read, buffer creation, tile write and output read failures identify stage, document, layer and region. Rollback remains enabled.

This checks one witness, not every output pixel or all seams. The precise original blank-layer cause and latest live result remain unconfirmed. If the next run fails, capture the complete panel message; do not remove verification, force alpha opaque, or silently commit empty output.

## 8. Evidence ledger

| Evidence | Status and limits |
|---|---|
| Release addon build/staging | Previously completed; addon and PDB remain present. No rebuild needed for current JS/doc edits. |
| C++ contracts and opt-in GPU CTest | Previously passed, including real RTX 4080 SUPER transfers at 17x3 and the four 1024-canvas tile sizes; no vendor DLL loaded. Not rerun during this documentation update. |
| Node diagnostic boundary tests | Three tests previously passed; separate from UXP host execution. |
| Photoshop native boundary checks | User confirmed six passing checks; explicitly no GPU inference. |
| Initial live canvas acceptance | User confirmed 1024x1024 RGB32 sRGB, seams at 960/960 at 100%/200%, one undo/redo, and no observed instability. Not proof of long-run leak freedom. |
| Current frontend automated tests | 25 tests passed after bounded verification changes; Photoshop is mocked. |
| Current preview/apply acceptance | Pending user retest after the regression patches. |
| Neural/CUDA/local-enhancement mode | Disabled/not accepted. |

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
cmake --build build --config Release --target gaslightr_uxpaddon GaslightrContractTests GaslightrGpuValidationTests
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Native validation failed' }
```

GPU tests are opt-in real device work. Frontend tests do not require Photoshop. Use the offline guide for SDK staging and native symbols. For live verification, select the original populated raster layer, test Preview, apply, inspect seams and alpha, then verify a single undo/redo. Preserve exact error messages and test-document profile/bit depth/bounds in the handoff.

## 10. Phase 8 choices — awaiting instruction

### Phase 8A: explicit Local Enhancement Mode

Retain validation as a separate diagnostic mode and keep `neuralInference: false`. Reuse the existing `pixels.js` 7x7 luminance box high-pass, intensity blend, style multipliers and smoothstep edge feathering with explicit local-enhancement labeling. Controls already exist in source but are bypassed in validation. Feathering is inward from rectangular layer bounds, not Photoshop selection/mask editing. Do not describe a CPU filter as DLSS, neural rendering, or compute shader work unless implemented as such. Preserve HDR range, original alpha, tile equivalence and one-step undo; measure latency before promising real time.

### Phase 8B: edge-case acceptance

Recommended order after current revision passes: 8/16/32-bit sources; non-sRGB profiles; sparse transparency and holes; fractional/negative/off-canvas bounds; odd and tiny dimensions; HDR/negative RGB within the FP16 contract; cancellation requested during a tile with deferred rollback; repeated runs and unload/memory observation. Test partial write failure and single-history rollback. Document expected versus observed profile/alpha semantics. Do not infer success from synthetic tests alone.

CUDA detail-only is outside both phases unless explicitly chosen later. Existing CUDA code needs repaired D3D12 sharing, texture mapping, device matching, synchronization and compatible luminance formats; no zero-color-shift guarantee has been established.

## 11. Reference hierarchy and maintenance

Historical references named by the user:

- `dlss_5_photoshop_plugin_ai_coding_blueprint.md`
- `photoshop-dlssnr-blueprint.md`
- `Photoshop DLSS 5 Plugin Architecture.md`

These were originally supplied under `D:\GitHub\Nexus\Advaet\Gaslightr`; they are not present at this repository root. Their inferred NGX ABI, universal SEH safety, registry-bypass and CUDA color-preservation claims are not implementation evidence. Consult them as design history when available, while preserving the hard disabled boundary above.

For continued work, update this root file with changed files, exact mode/contract, test evidence, host results, and remaining blockers. Keep the docs pointer intact; do not maintain divergent full copies. Do not overwrite pre-existing user or uncommitted implementation changes during documentation maintenance.
