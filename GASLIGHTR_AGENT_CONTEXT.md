# Gaslightr: Agent Architecture & Handoff Context Document

> **Project Identity:** **Gaslightr**  
> **Platform:** Windows 10/11 x64  
> **Host Environment:** Adobe Photoshop 2024+ (UXP v7.0+)  
> **Engine:** Headless Direct3D 12 + NVIDIA DLSS 5 Neural Rendering (`nvngx_dlssnr.dll`) + CUDA Toolkit 12.x  
> **Primary Goal:** Mathematically "gaslight" the NVIDIA DLSS 5 Neural Rendering real-time pipeline into processing static 2D Photoshop layers by synthesizing a headless Direct3D 12 temporal game loop with synthetic camera jitter, inverse motion vectors, and flat infinite depth buffers.

---

## 1. Master Reference Documentation

Any incoming agent (e.g. OpenAI Codex) must anchor its work to the three master reference files:

1. **`dlss_5_photoshop_plugin_ai_coding_blueprint.md`**
   - *Master Step-by-Step Blueprint*: Roadmap from Phase 0 to Phase 7. Specifies memory boundaries, warmup iterations, multi-pass rendering loops, and teardown constraints.
2. **`photoshop-dlssnr-blueprint.md`**
   - *NGX & Hooking Specifications*: Defines unexported structural layouts (`NVNGX_InitializationParams`, `NVNGX_EvaluationInputs`), function pointers (`PFN_NVNGX_INITIALIZE_NR`, `PFN_NVNGX_EVALUATE_NR`), Win32 SEH safety nets, and headless D3D12 device instantiation.
3. **`Photoshop DLSS 5 Plugin Architecture.md`**
   - *Memory Alignment & Color Science*: Specifies the bitwise 256-byte pitch alignment formula, Windows Registry bypass keys (`ALLOW_UNSIGNED`), and CUDA CIELAB ($L^*a^*b^*$) detail-only luminance decomposition matrices.

---

## 2. Architectural Blueprint & Current State

```
[Photoshop SDK / UXP Frontend]
       │
       │ (Phase 5 / 6: JSON payload via messaging.sendSDKPluginMessage)
       ▼
[C++ Native Bridge / N-API]
       │
       │ (Phase 6: napi_get_typedarray_info zero-copy pointer handoff)
       ▼
[MemoryBridge: 256-byte Row Pitch Alignment & Normalization]
       │
       │ Upload Buffer (D3D12_HEAP_TYPE_UPLOAD)
       │ CopyTextureRegion ──► VRAM Texture (D3D12_HEAP_TYPE_DEFAULT: R16G16B16A16_FLOAT)
       ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Detail-Only Mode Toggle (CUDAInterop.cu)                               │
│  - Export DX12 texture via IDXGIResource1::CreateSharedHandle          │
│  - cudaImportExternalMemory                                            │
│  - Linear RGB → D65 XYZ → CIELAB Kernel                                │
│  - Extract L* (Luminance R16_FLOAT) → sent to DLSS                     │
│  - Freeze a*, b* (Chroma R16G16_FLOAT) in VRAM                         │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ PipelineSimulator: Temporal Spoofing Game Loop                         │
│  - Synthetic Depth Buffer: DXGI_FORMAT_R32_FLOAT = 1.0f (far plane)    │
│  - Synthetic Exposure: 1x1 DXGI_FORMAT_R16_FLOAT = 1.0f (SDR neutral)  │
│  - Radical-Inverse Halton Sequence: Base 2 (X), Base 3 (Y)             │
│  - Motion Vector Texture: Exact Inverse (-jitterX, -jitterY)           │
│                                                                        │
│  [Stage 1: Temporal Warmup Loop]                                       │
│   • Iterates 24–32 frames                                              │
│   • ResetHistory = true ONLY on frame 0                                │
│   • WaitForGPU() fence synchronization after every frame               │
│                                                                        │
│  [Stage 2: Multi-Pass Ping-Pong Enhancement]                           │
│   • Iterates 1–4 passes, ping-ponging between VRAM textures            │
│   • Feeds pass N output as pass N+1 input                              │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Chroma Recombination (If Detail-Only Mode)                             │
│  - CUDA Kernel: Enhanced L* + Original (a*, b*) → D65 XYZ → Linear RGB │
│  - Result: 100% preservation of original color grading & skin tones    │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
[Readback Buffer: D3D12_HEAP_TYPE_READBACK]
       │
       │ (Phase 6: PSBufferSuite writeback to active Photoshop layer)
       ▼
[Photoshop Canvas Target Layer]
```

---

## 3. Implemented Components & File Registry

### 3.1. Build & Core Structs
- **[`CMakeLists.txt`](CMakeLists.txt)**
  - Configures Windows x64 MSVC compilation with C++20 (`/std:c++20`), strict `/W4 /WX`, static runtime (`/MT` / `/MTd`).
  - Links system graphics libraries: `d3d12.lib`, `dxgi.lib`, `dxguid.lib`, `Advapi32.lib`.
- **[`include/nvngx_dlssnr_core.h`](include/nvngx_dlssnr_core.h)**
  - Exact unexported NGX struct definitions extracted from `photoshop-dlssnr-blueprint.md`:
    - `NVNGX_InitializationParams`: `Width`, `Height`, `TargetWidth`, `TargetHeight`, `PerformanceMode`.
    - `NVNGX_EvaluationInputs`: `pColorInput`, `pMotionVectors`, `pDepthBuffer`, `MotionVectorScaleX/Y`, `CameraJitterX/Y`, `FrameIndex`, `ResetHistory`.
    - Function pointer typedefs: `PFN_NVNGX_INITIALIZE_NR`, `PFN_NVNGX_EVALUATE_NR`.

### 3.2. Headless Direct3D 12 Engine
- **[`src/DX12Context.h`](src/DX12Context.h) & [`src/DX12Context.cpp`](src/DX12Context.cpp)**
  - **Programmatic Registry Bypass (`AssertRegistryBypass`)**:
    - Writes `AllowUnsignedBinaries = 1` and `OverrideFeatureDenial = 1` to `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore`.
    - Writes `DisableNgxSignatureChecks = 1` to `HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm`.
    - Bypasses the NVIDIA driver's cryptographic binary signature check.
  - **Discrete GPU Selection**: Uses `IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` to locate NVIDIA discrete hardware (Vendor ID `0x10DE`).
  - **Headless Pipeline**: Creates an `ID3D12Device` (Feature Level 12_0), a `D3D12_COMMAND_LIST_TYPE_DIRECT` Command Queue, Command Allocator, Command List, and CPU-GPU synchronization Fence (`WaitForGPU()`). Zero swapchains.

### 3.3. Dynamic NGX Loader & Safety Net
- **[`src/NeuralBackend.h`](src/NeuralBackend.h) & [`src/NeuralBackend.cpp`](src/NeuralBackend.cpp)**
  - Thread-safe singleton managing runtime lifecycle of `nvngx_dlssnr.dll`.
  - Dynamically loads module via `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`.
  - Resolves entry points via `GetProcAddress`:
    - Primary: `NVSDK_NGX_DLSSNR_InitializeNR`, `NVSDK_NGX_DLSSNR_EvaluateNR`.
    - Alternate: `NVSDK_NGX_InitializeNR`, `NVSDK_NGX_EvaluateNR`.
  - **Host Safety Net**: Wraps all dynamic entry points in Win32 Structured Exception Handling (`__try` / `__except`) to prevent access violations or struct layout mismatches from crashing the host Adobe Photoshop process. Returns graceful error codes.

### 3.4. Memory Alignment & Format Normalization
- **[`src/MemoryBridge.h`](src/MemoryBridge.h) & [`src/MemoryBridge.cpp`](src/MemoryBridge.cpp)**
  - **256-Byte Pitch Alignment**:
    $$\text{AlignedPitch} = (\text{Width} \times \text{BytesPerPixel} + 255) \;\&\; \sim255$$
    Prevents D3D12 texture shearing, distorted reads, and hardware driver access violations.
  - **Three-Tier Staging**: Manages `D3D12_HEAP_TYPE_UPLOAD`, `D3D12_HEAP_TYPE_DEFAULT` (VRAM), and `D3D12_HEAP_TYPE_READBACK`.
  - **Format Normalization**: Normalizes incoming 8-bit, 16-bit, and 32-bit pixel channels into `DXGI_FORMAT_R16G16B16A16_FLOAT` using a precomputed 256-entry IEEE-754 half-float LUT (for 8-bit) and scalar normalization (for 16-bit). DLSS requires linear float precision to prevent highlight clipping.

### 3.5. Temporal Game-Loop Simulator
- **[`src/PipelineSimulator.h`](src/PipelineSimulator.h) & [`src/PipelineSimulator.cpp`](src/PipelineSimulator.cpp)**
  - **Radical-Inverse Halton Sequence Generator**:
    - Generates sub-pixel jitter coordinates using Base 2 (X) and Base 3 (Y), shifted by `-0.5f` and normalized by tile dimensions.
  - **Synthetic Inputs**:
    - Depth buffer: Uniform `1.0f` (`DXGI_FORMAT_R32_FLOAT`) representing infinite far plane.
    - Exposure buffer: 1x1 `1.0f` (`DXGI_FORMAT_R16_FLOAT`) neutral SDR.
    - Motion vectors: `DXGI_FORMAT_R16G16_FLOAT` containing $(-\text{jitterX}, -\text{jitterY})$. The exact inverse of camera jitter tricks the network into detecting camera translation over a static scene.
  - **Warmup Loop**: Iterates 24–32 frames. `ResetHistory = true` strictly on frame 0. Calls `WaitForGPU()` after every frame.
  - **Multi-Pass Loop**: Implements ping-pong buffer refinement (1–4 passes) feeding output $N$ into input $N+1$.

### 3.6. CUDA CIELAB Detail-Only Mode
- **[`src/CUDAInterop.h`](src/CUDAInterop.h) & [`src/CUDAInterop.cu`](src/CUDAInterop.cu)**
  - **Shared Memory Handles**: Exports DX12 textures via `IDXGIResource1::CreateSharedHandle` and imports them into CUDA via `cudaImportExternalMemory`.
  - **Decomposition Kernel (`kernelRGBtoCIELAB`)**:
    - Converts Linear RGB $\to$ D65 XYZ $\to$ CIELAB ($L^*a^*b^*$).
    - Extracts $L^*$ (Luminance) into `R16_FLOAT` as DLSS input.
    - Stores $a^*$ and $b^*$ (Chroma) in VRAM.
  - **Recombination Kernel (`kernelCIELABtoRGB`)**:
    - Recombines DLSS-sharpened $L^*$ with preserved $a^*b^*$ and converts back to Linear RGB.
    - Eliminates chromatic aberration, tint shifts, and color drift on sensitive skin tones.

---

## 4. Pending Work: Phase 5 & Phase 6

To bring Gaslightr into full production deployment inside Adobe Photoshop, the following phases remain to be implemented:

### Phase 5: UXP Frontend & IPC Bridge
- Directory: `/uxp-frontend`
- **`manifest.json`**: UXP v7.0+ hybrid plugin manifest specifying permissions, minimum Photoshop version, and UI panels.
- **`index.jsx`**: React + Spectrum Web Components UI:
  - `sp-slider` / `sp-number-field`: **NR Intensity** ($0.0 \to 1.0$), **Local Tone**, **Mask Feathering**.
  - `sp-picker`: **NR Style** (Default, Natural, Cinematic).
  - `sp-checkbox`: **Detail-Only Mode** (triggers CIELAB kernel).
  - Interactive Preview: HTML `<canvas>` rendering localized crop of active viewport.
- **`bridge.js`**: Messaging client calling `photoshop.action.executeAsModal` and sending JSON configurations via `messaging.sendSDKPluginMessage()`.

### Phase 6: Photoshop C++ SDK & Node-API Binding
- **Plugin Entry Points**: Transition `src/main.cpp` into a DLL exporting `AutoPluginMain` for Photoshop and registering `UXPMessageHandler` via `sUxpProcs->AddUXPMessageListener`.
- **Tile-Based Document Extraction**:
  - Implement `PSImageTileSuite` and `PSBufferSuite` to extract document layers in discrete 1024x1024 or 2048x2048 chunks, respecting maximum VRAM working ceilings.
- **Node-API Zero-Copy Handshake**:
  - Connect `napi_get_typedarray_info` to map JavaScript ArrayBuffers directly into `MemoryBridge::UploadToGPU` without copying through system memory.
- **Layer Writeback**:
  - Write final readback pixels into a new target layer inside the active document upon modal completion.
