# Master Codex Prompt: DLSS 5 Photoshop Hybrid Plugin (Gaslightr)

You can copy and paste the prompt below directly into OpenAI Codex or any advanced coding agent.

---

```markdown
Role & Objective:
You are an expert C++ Graphics Programmer and Adobe UXP Hybrid Plugin Developer. We are building "Gaslightr", a local Adobe Photoshop hybrid plugin. The core premise of Gaslightr is to mathematically "gaslight" the NVIDIA DLSS 5 Neural Rendering engine into processing static 2D image layers by synthesizing a fake, headless Direct3D 12 temporal game loop.

Context & Master References:
You must strictly consult the three primary reference documents attached in this workspace:
1. `dlss_5_photoshop_plugin_ai_coding_blueprint.md` (Master step-by-step roadmap).
2. `photoshop-dlssnr-blueprint.md` (Exact unexported NGX structs, Win32 hooking, headless D3D12 device, Halton sequence).
3. `Photoshop DLSS 5 Plugin Architecture.md` (256-byte pitch alignment, registry bypass flags, and CUDA CIELAB interop theory).
Also review `GASLIGHTR_AGENT_CONTEXT.md` for full implementation status and component breakdown.

Project Current State (What Is Already Built):
- Build System: CMakeLists.txt configured for Windows x64 MSVC C++20 with static runtime (/MT), linking d3d12, dxgi, dxguid, Advapi32.
- NGX Layouts (`include/nvngx_dlssnr_core.h`): Structs NVNGX_InitializationParams, NVNGX_EvaluationInputs, and function pointer typedefs PFN_NVNGX_INITIALIZE_NR, PFN_NVNGX_EVALUATE_NR.
- Headless D3D12 Context (`src/DX12Context.h`, `src/DX12Context.cpp`): 
  * Programmatic ALLOW_UNSIGNED registry override (HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore and HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm) to bypass NVIDIA driver signature verification.
  * Adapter enumeration selecting discrete NVIDIA hardware (Vendor ID 0x10DE).
  * Headless D3D12 Device, DIRECT Command Queue, Allocator, List, and Fence sync (WaitForGPU()). No swapchain.
- Neural Backend Hooking (`src/NeuralBackend.h`, `src/NeuralBackend.cpp`):
  * Dynamic LoadLibraryExW of nvngx_dlssnr.dll.
  * GetProcAddress symbol resolution with fallback variants.
  * Win32 Structured Exception Handling (__try / __except) around all DLL calls to prevent crashes in host processes due to struct mismatches.
- Memory Staging & Alignment (`src/MemoryBridge.h`, `src/MemoryBridge.cpp`):
  * D3D12 256-byte pitch alignment: AlignedPitch = (Width * BytesPerPixel + 255) & ~255.
  * Three-tier heap architecture: UPLOAD, DEFAULT (VRAM), READBACK.
  * Format normalization to DXGI_FORMAT_R16G16B16A16_FLOAT via IEEE-754 half-float precomputed LUT (for 8-bit) and dynamic float conversion to prevent HDR highlight clipping.
- Temporal Spoofing Game Loop (`src/PipelineSimulator.h`, `src/PipelineSimulator.cpp`):
  * Radical-inverse Halton Sequence generator (Base 2 for X, Base 3 for Y).
  * Synthetic depth buffer: R32_FLOAT uniformly at 1.0f (far plane).
  * Synthetic motion vectors: R16G16_FLOAT uniformly at (-jitterX, -jitterY). Exact inverse of sub-pixel jitter tricks DLSS into detecting camera movement over a static scene.
  * 24-frame temporal warmup loop (ResetHistory = true on frame 0 only, WaitForGPU() per frame).
  * 1–4 pass ping-pong multi-pass enhancement loop.
- CUDA CIELAB Detail-Only Mode (`src/CUDAInterop.h`, `src/CUDAInterop.cu`):
  * Shared NT handle interop via IDXGIResource1::CreateSharedHandle -> cudaImportExternalMemory.
  * CUDA kernel decomposing linear RGB -> D65 XYZ -> CIELAB.
  * Isolates L* (luminance) into R16_FLOAT for DLSS structural enhancement while keeping a*, b* (chroma) safe in VRAM.
  * Recombination kernel merging enhanced L* + original a*b* -> linear RGB, guaranteeing zero color shift or skin-tone drift.

Your Immediate Assignment: Phase 5 (UXP Frontend & IPC Bridge) and Phase 6 (Photoshop C++ Native Binding)

Execute the following without cutting corners or leaving placeholders:

1. Phase 5: UXP Frontend Construction:
   - Create directory `/uxp-frontend`.
   - `manifest.json`: Valid UXP v7.0+ hybrid plugin manifest defining permissions, required host version (Photoshop 2024+), and entry point.
   - `index.jsx`: UI built with Spectrum Web Components (or pure React + Spectrum web components) containing:
     * Sliders and number inputs for: NR Intensity (0.0 to 1.0), Local Tone / Structure (0.0 to 1.0), Mask Feathering.
     * Dropdowns for: NR Style (Default, Natural, Cinematic) and DLSS Performance Preset.
     * Toggle switch for: Detail-Only Mode (CIELAB Chroma Preservation).
     * Interactive HTML <canvas> for Real-Time Viewport Preview.
   - `bridge.js`: Messaging interface wrapping all calls in `photoshop.action.executeAsModal` and dispatching JSON payloads via `messaging.sendSDKPluginMessage()`.

2. Phase 6: Photoshop C++ SDK & Node-API Binding:
   - Create the native plugin interface exporting `AutoPluginMain` and registering a `PSUXPSuite1` listener (`sUxpProcs->AddUXPMessageListener`).
   - Implement `PSImageTileSuite` logic to extract layer pixel data in bounded chunks (1024x1024 or 2048x2048 tiles) to respect GPU VRAM ceilings.
   - Implement Node-API (N-API) zero-copy buffer passing (`napi_get_typedarray_info`) from JS TypedArrays into `MemoryBridge::UploadToGPU`.
   - Implement `PSBufferSuite` writeback to create/populate a target enhanced layer in Photoshop upon modal execution completion.

Execution Constraints:
- Production-Ready C++ & JS: No "TODO"s, no pseudo-code, and no stubbed math.
- Thread Safety: Ensure Photoshop DOM access is strictly modal-wrapped, and all GPU queues are completely flushed (WaitForGPU()) before returning buffers to the JavaScript runtime.
- Preserve Architecture: Adhere strictly to the existing classes: `DX12Context`, `NeuralBackend`, `MemoryBridge`, `PipelineSimulator`, and `CUDAInterop`.
```
