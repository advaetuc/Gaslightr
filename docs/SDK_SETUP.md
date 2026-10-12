# Gaslightr SDK setup

Installing the SDKs is the next step, not the final step: the UXP native adapter, Photoshop resource packaging and real-runtime validation still need implementation afterward. The graphics core and Node diagnostic binding already build; see [the exact status](IMPLEMENTATION_STATUS.md).

## 1. Confirm the Windows toolchain

Use Windows x64 and Visual Studio 2022 with **Desktop development with C++**, the MSVC v143 compiler, a Windows 10/11 SDK, and CMake tools. These were found and used successfully on this computer. Open **Developer PowerShell for VS 2022** to put `cmake` and compiler tools on PATH. Keep x64, C++20 and `/MT` consistent across native targets.

From the project directory:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
& 'C:\Program Files\nodejs\node.exe' --test tests/frontend.test.js
```

## 2. Obtain both Adobe SDKs

Sign in with your Adobe account at the [Adobe Developer Console](https://developer.adobe.com/console/servicesandapis). Find the Photoshop developer resources and download **Photoshop C++ SDK** and **UXP Hybrid Plugin SDK**. They are separate packages. Adobe links the downloads from its [Photoshop developer page](https://developer.adobe.com/photoshop/) and [hybrid getting-started guide](https://developer.adobe.com/photoshop/uxp/2022/guides/hybrid-plugins/getting-started/). Accept the applicable SDK terms yourself.

Extract them into stable directories, for example:

```text
C:\SDKs\Adobe\Photoshop\
C:\SDKs\Adobe\UXP-Hybrid\
```

Keep each archive's full directory structure. Record the version and README of each SDK. The Photoshop package must include `PIUXPSuite.h`, `PIAction.h`, `SPBasic.h`, `SPInterf.h`, `SPMessag.h` and automation plugin samples. The hybrid package must include `UxpAddon.h`, `UxpAddonShared.h`, `UxpAddonTypes.h`, `template-dev` and `template-plugin` or their version-specific equivalents.

Once extracted, provide the two root paths so the remaining adapter can be built against the actual headers. No Adobe SDK paths are assumed by this repository.

## 3. Configure Photoshop and UXP Developer Tool

Install Photoshop 2024 (25.x) or later and UXP Developer Tool through Adobe's supported distribution. This project targets Photoshop 25.0 or later. Adobe's hybrid minimum is Photoshop 24.2 and UDT 1.7, but a newer SDK may raise that minimum: follow its bundled README. [Adobe version requirements](https://developer.adobe.com/photoshop/uxp/guides/hybrid-plugins/).

In Photoshop, enable plugin developer mode in Preferences → Plugins, then restart if requested. Load the SDK's **unmodified `template-plugin`** with UDT and confirm its sample native function works. Build **`template-dev`** in Visual Studio x64, then repeat with your compiled sample. This verifies the SDK, host and binary layout before integrating Gaslightr.

## 4. Compile-check the Photoshop messaging entry point

Use the SDK sample project's include directories. One way to discover the relevant directories in the extracted SDK is:

```powershell
$psSdkRoot = 'C:\SDKs\Adobe\Photoshop'
$sdkIncludes = (Get-ChildItem -LiteralPath $psSdkRoot -Recurse -Filter '*.h' |
    Select-Object -ExpandProperty DirectoryName -Unique) -join ';'
cmake -S . -B build -DGASLIGHTR_CHECK_PHOTOSHOP_SDK=ON "-DPHOTOSHOP_SDK_INCLUDE_DIRS=$sdkIncludes"
cmake --build build --config Release --target GaslightrPhotoshopMessaging
```

This is a **compile check**, not an installable Photoshop plugin. Use the SDK automation sample's resource/PiPL build to package `AutoPluginMain`, with `PIComponentProperty` identifying `com.advaet.gaslightr.native`. Match the sample's persistent startup/shutdown behavior so the listener exists before UXP sends a message. Do not omit resource compilation or rename the object file into a plugin. [Adobe messaging and entry points](https://developer.adobe.com/photoshop/uxp/2022/ps-reference/media/cpp-pluginsdk).

The requested C++ tile/writeback suites need inspection at this point. The frontend already has a documented Imaging API path. If the installed C++ SDK does not expose the requested suite, choose a documented filter/channel-port or Imaging API path explicitly rather than defining a fictitious suite.

## 5. Build the actual UXP addon after implementing its adapter

Use the installed hybrid SDK template to create `gaslightr.uxpaddon`, link `GaslightrCore`, and register the following interface using Adobe's addon API:

```text
capabilities() -> { protocol: 1, detailOnly: boolean,
                    detailOnlyReason: string, sdkMessaging: boolean }
processTile(Float32Array, { width, height, preset, detailOnly }) -> Float32Array
```

The JS bridge also accepts a Promise from `processTile`. If it is asynchronous, retain the input using the SDK's actual persistent-reference mechanism, ensure no mutation/detachment can race GPU work, and resolve only after fences complete. Photoshop C++ suite access must run on the host main thread using the SDK scheduling API; a modal JS scope alone does not move native callbacks to that thread. See [Adobe's threading model](https://developer.adobe.com/uxp/guides/how-to/hybrid-plugins/build).

The intended Windows bundle location is `uxp-frontend/win/x64/gaslightr.uxpaddon`. Match your SDK sample's manifest schema and addon declaration. The supplied panel manifest uses schema 5 for the Photoshop 25 baseline and declares `enableAddon`; Adobe's newer cross-host build guide specifies schema 6. Verify the combination against the selected Photoshop/SDK versions before packaging, adjusting the minimum host version if needed. UXP runtime version 7 is not manifest schema version 7. [Manifest reference](https://developer.adobe.com/uxp/guides/explanation/concepts/manifest/).

Only after the actual addon is present should you load Gaslightr's `uxp-frontend/manifest.json` in UDT. No transpiler is needed: `index.jsx` deliberately contains ordinary JavaScript using built-in Spectrum controls. A missing-addon error is expected before this step. **Renaming the tested `gaslightr.node` binary will not create a UXP addon.**

## 6. Node diagnostic binding, already verified locally

Official Node v24.19.0 headers and `node.lib` were downloaded into ignored `vendor/node` to match the installed Node runtime. They are not Adobe SDKs or runtime deliverables. To rebuild this diagnostic target:

```powershell
cmake -S . -B build -DGASLIGHTR_BUILD_NODE_BINDING=ON `
  "-DNODE_API_INCLUDE_DIR=$PWD/vendor/node/node-v24.19.0/include/node" `
  "-DNODE_IMPORT_LIBRARY=$PWD/vendor/node/node.lib"
cmake --build build --config Release
& 'C:\Program Files\nodejs\node.exe' --test tests/native.test.js
```

These tests exercise the actual compiled N-API boundary without running a neural DLL. Pixel input is borrowed synchronously; GPU work cannot outlive the JS handle. The tests reject shared memory, invalid ranges, malformed dimensions and incorrect buffer sizes, and check nonzero TypedArray offsets.

## 7. Neural runtime and CUDA prerequisites

Obtain the intended neural runtime and its authoritative ABI definitions. Verify exported functions, calling conventions, struct sizes, resource-state rules, and shutdown/feature-destruction APIs before calling it inside Photoshop. The supplied structs were preserved but are inferred, not validated vendor interfaces. Place the verified `nvngx_dlssnr.dll` and its required dependencies beside the native module. Start with a standalone diagnostic process and a small test image.

For Detail-Only mode, install a CUDA Toolkit compatible with your MSVC compiler and NVIDIA driver from [NVIDIA's CUDA downloads](https://developer.nvidia.com/cuda-downloads), then repair and test `CUDAInterop.cu` as listed in the status file. Installation alone does not make the existing interop code correct. Enable `detailOnly` only after RGB→Lab→RGB, alpha, resource sharing and synchronization tests pass. Use the [CUDA graphics interoperability documentation](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html) for the D3D12 memory layout and handle contract.

The panel processing path does not modify machine registry settings. The original registry-writing harness remains opt-in and was not run during this work. Its inferred driver flags and the effect of any override still need validation for the actual runtime.

## 8. First end-to-end acceptance run

On a disposable document, compare 8-, 16- and 32-bit sources, transparent pixels, non-sRGB profiles, negative layer offsets and odd sizes spanning several tiles. Confirm one new layer and one undo step, and that cancellation or failure on a later tile removes the entire output. Inspect tile seams, compare the crop preview to full output, and profile latency and VRAM over repeated runs. Exercise unload/reload and device-loss behavior. Only then assess production readiness.
