# Implementation status — 12 October 2026

## 13 October: GPU validation mode

This update supersedes the earlier runtime/host status below. The user confirmed Photoshop addon loading and all six native boundary checks. The neural backend is now disabled before DLL loading; `PipelineSimulator` performs explicit GPU copies through the temporal and ping-pong loops. Capabilities report `neuralInference: false` and `executionMode: d3d12-validation-copy`. JS postprocessing is bypassed and the UI/output/history are labeled validation. Input/output remains RGBA32F with RGBA16F GPU storage and original alpha restoration.

Release addon and matching PDB are staged. Both CTests (including real GPU transfers on the RTX 4080 SUPER) and all 18 frontend/Node tests passed. Both requested copies of the neural DLL are present with identical hashes but are not loaded by this path. Live Photoshop canvas, profile, seam and undo acceptance remains pending; follow [Phase 7 instructions](PHASE7_GPU_VALIDATION.md).

The Adobe SDKs are now installed in `C:\SDKs\Adobe\Photoshop` and `C:\SDKs\Adobe\UXP-Hybrid`. A real Adobe UXP adapter has been compiled and staged; the [offline implementation guide](OFFLINE_UXP_IMPLEMENTATION_GUIDE.md) supersedes the earlier SDK installation instructions. None of the tests establish successful DLSS 5 inference or Photoshop host integration.

## Implemented and checked

| Component | Coverage |
| --- | --- |
| `uxp-frontend` | Spectrum controls, linked numeric inputs, preset/style selection, CIELAB availability message, debounced center-crop preview, draggable before/after canvas, cancellation and status |
| UXP bridge | `photoshop.core.executeAsModal`, correlated SDK messages with timeouts/cleanup, Imaging API extraction and writeback, one history transaction with rollback |
| Tiling | At most 1024×1024 input tiles, 32-pixel halos, non-overlapping 960×960 cores, clipped edge tiles, negative layer coordinates, bounded per-tile ownership |
| `TileProcessor` | Linear RGBA32F validation, GPU-budget preflight, existing `MemoryBridge`/`PipelineSimulator` orchestration, float16 readback conversion, original alpha retention, serialized NGX access |
| `NodeBinding` | Real `napi_get_typedarray_info` buffer borrow, byte-offset handling, native exceptions translated to JS errors, compiled Windows x64 `.node` diagnostic module |
| `UxpAddonAdapter` | Actual Adobe API registration, intrinsic Float32Array metadata validation, synchronous ArrayBuffer borrowing, `TileProcessor` execution, checked core fences, JS exceptions and owned output buffers; compiled x64 `.uxpaddon` with matching PDB |
| Offline bring-up | SDK staging script, CMake target with `/Y-` on all Adobe helpers, manifest 6 as required by this SDK, in-panel native boundary checks, directory-junction verification and native-debugger SOP |
| Existing graphics core | Fixed MSVC SEH/unwinding compile error, UAV-capable output textures and transitions, checked reset/close/fence results, device-removal reporting |
| `PhotoshopMessaging` | SDK-dependent `AutoPluginMain`, suite acquisition/release, listener lifecycle and bounded configuration-envelope receipts; **not compiled or host-tested** |

Validation completed locally: MSVC 19.44, Windows SDK 10.0.26100, C++20, static CRT, `/W4 /WX`; 12 JavaScript frontend tests, 3 tests against the compiled Node binding, and the C++ pixel-contract test executable. Tests use a mock Photoshop host, exercise rollback/cancellation/seams, and do not invoke the neural DLL. UI layout has not been inspected inside Photoshop.

## Exact pixel and control contract

The adapter accepts a chunky `Float32Array` of `width * height * 4` components, linear sRGB/D65, straight alpha. Width and height are 1–2048. Input values must be finite and within the finite half-float range; alpha is 0–1. UXP requests RGB, sRGB, 32-bit pixels from Photoshop, expands opaque RGB to RGBA when necessary, and writes a new raster layer using the same explicit profile. The source layer remains intact. Output rasterization does not duplicate source blend modes, masks, layer effects, clipping relationships, or smart-object semantics; host tests must establish intended behavior for those cases.

The native call borrows the JS view synchronously, preventing the calling isolate from detaching or changing it while processing. SharedArrayBuffer input is rejected. GPU work is fenced before the call returns or resource cleanup runs. This eliminates an intermediate input copy at the JS/C++ boundary; normalization into the upload heap, GPU transfers, readback conversion, and creation of the output JS buffer still involve copies. A `.node` binary is not loadable as a `.uxpaddon` by renaming it.

NR Intensity blends enhanced and original pixels. Default, Natural, and Cinematic use blend multipliers 1, 0.65, and 1.25 respectively. Local Tone / Structure adds a luminance high-pass signal from a 7×7 box filter. Mask Feathering applies a smoothstep enhancement mask inward from the source layer's rectangular edges, in pixels; it does not feather Photoshop's selection or an existing layer mask. These are explicitly defined post-inference controls, not claimed fields in the undocumented NGX ABI. Preset is passed to the existing initialization struct as 0/1/2.

The 32-pixel halo covers the local postprocessing filter. It does **not** prove seam-free neural inference: the model's receptive field and independent tile histories still need image-based evaluation. Preview uses a 512-pixel full-resolution center crop, not the current Photoshop viewport. Processing is debounced; real-time latency has not been measured. The synchronous native call cannot be interrupted mid-tile.

## Remaining work and blockers

1. **Adobe UXP host validation:** the adapter now builds against the installed SDK 0.1.0 and exports `uxp_addon_init` / `uxp_addon_terminate`. Validate discovery, addon loading, the six in-panel boundary checks and typed-array intrinsics inside Photoshop. The existing junction points at the frontend, but host registration has not been established. The requested `photoshop.log` was absent during inspection. The Node binding remains a separate diagnostic module.
2. **Optional Photoshop C++ companion:** compile `PhotoshopMessaging.cpp` with the actual headers and package the automation sample's PiPL/resource target only if a separate SDK companion is needed. The current CMake option only builds an object file. The listener acknowledges delivery; it does not perform inference or write pixels. The new direct addon reports `sdkMessaging: false` and does not require this companion.
3. **C++ pixel suites:** the references supply no declarations or usable sample for `PSImageTileSuite`. Its availability/signature and a supported C++ layer-write path must be established from the installed SDK. `PSBufferSuite` allocation alone is not an implemented layer-write path here. Current extraction/writeback uses Adobe's documented UXP Imaging API. No invented suite structs were added.
4. **CUDA detail mode:** existing `CUDAInterop.cu` is retained but not built or called. It attempts an `IDXGIResource1` path for D3D12 resources, maps tiled textures as linear CUDA buffers, and its single-channel luminance does not match the simulator's RGBA output contract. It also takes the address of a temporary resource description. Repair resource sharing, array/surface mapping, adapter matching, synchronization, alpha handling, and luminance format conversion before enabling the capability.
5. **NGX ABI and teardown:** the supplied blueprint itself labels its structs as inferred. Their compatibility with an actual `nvngx_dlssnr.dll` has not been established. The existing backend unloads the module but has no verified feature-destruction/shutdown entry point. Repeated tile initialization must be tested for leaks after the real runtime contract is obtained. SEH catches some faults but cannot guarantee host-process safety after arbitrary ABI mismatch.
6. **Production host validation:** validate minimum Photoshop version, SDK-specific manifest/addon layout, HDR/color conversion, clipped/transparent tiles, rollback, cancellation, neural seams, long-running memory usage, and plugin unloading inside Photoshop.

The original `GASLIGHTR_AGENT_CONTEXT.md` predates these checks and describes CUDA/NGX functionality more confidently than the available evidence supports. Use this status file alongside it.

## Verified API references

- Adobe documents modality under [`core.executeAsModal`](https://developer.adobe.com/photoshop/uxp/2022/ps-reference/objects/options/executioncontext), including rollback through `resumeHistory(..., false)`.
- Pixel extraction, explicit profiles, float buffers, disposal and raster-layer insertion use the [Imaging API](https://developer.adobe.com/photoshop/uxp/2022/ps-reference/media/imaging).
- The actual suite spelling and messaging pattern are in [C++/UXP messaging](https://developer.adobe.com/photoshop/uxp/2022/ps-reference/media/cpp-pluginsdk).
- Adobe's [hybrid plugin build guide](https://developer.adobe.com/uxp/guides/how-to/hybrid-plugins/build) describes a separate native addon API and thread model.
