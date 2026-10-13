# Gaslightr: offline UXP Hybrid Addon setup and implementation

**13 October update:** the user confirmed addon loading and six native boundary checks inside Photoshop. The current build now uses explicit D3D12 copy validation with neural inference disabled. For the next canvas test, use [Phase 7 GPU validation](PHASE7_GPU_VALIDATION.md); its instructions supersede the neural-runtime prerequisite and enhancement expectations below. Earlier build sizes and verification counts describe the original build.

Validated against the repository and locally installed SDK on **12 October 2026**.

This manual uses local files, CMake, MSVC, Photoshop and Visual Studio's native debugger. It requires no developer port, remote-debugging session, package download, Creative Cloud application, UXP Developer Tool, or GUI packaging tool. The Adobe SDKs and compiler must already be installed.

**Build validation is complete; Photoshop execution validation is not.** The accompanying adapter was compiled against `C:\SDKs\Adobe\UXP-Hybrid\`, staged as a real x64 `.uxpaddon`, and inspected for its two required exports. Photoshop was not launched, attached to, or modified during preparation. The neural DLL is still a separate, unvalidated prerequisite.

Local verification passed: the Release x64 addon build, the C++ pixel-contract CTest, and all 12 frontend tests. The built and staged addon SHA-256 hashes match. The dependency table contains Windows graphics/system libraries and no Node runtime. These checks do not exercise Adobe's scripting runtime or the neural DLL.

## 1. Repository directory structure and file context

### 1.1 Corrections established from the files on disk

| Requested assumption | What the installed files actually establish |
| --- | --- |
| Core sources already live in `src/core/` | They live directly in `src/`. This guide preserves that layout; it does not require a risky source move. |
| Manifest Schema 5 supports this hybrid SDK | The installed SDK README explicitly requires **Manifest 6 or later**, and its sample uses 6 with Photoshop 24.2+. The supplied manifest is corrected to 6 while retaining the requested **Photoshop 25.0** minimum. Keeping schema 5 would contradict this SDK's documented load contract. |
| Adobe supplies `UXP_ADDON_FUNCTION` | This SDK has no such macro. It supplies `UXP_ADDON_INIT`, `UXP_ADDON_TERMINATE`, `uxp_addon_create_function` and `uxp_addon_set_named_property`. The adapter below uses those real interfaces. |
| UXP exposes Node's typed-array-info API | The installed `HYBRID_PLUGIN_SDK_VERSION` is `0.1.0`; its table has no `uxp_addon_get_typedarray_info`. The adapter reads intrinsic typed-array metadata, then uses its real `uxp_addon_get_arraybuffer_info`. Do not append invented fields to Adobe's ABI table. |
| A junction still needs creating | `%APPDATA%\Adobe\UXP\Plugins\External\Gaslightr` already points to this repository's frontend. The commands below verify and reuse it. |
| The designated host log already exists | `%APPDATA%\Adobe\UXP\logs\photoshop.log` did not exist when inspected. Monitoring it is part of host bring-up, not evidence the plugin has loaded. |

These corrections are necessary for an executable guide. The requested schema-5/`UXP_ADDON_FUNCTION` combination cannot truthfully be presented as supported by the installed SDK. Adobe's [hybrid build instructions](https://developer.adobe.com/uxp/guides/how-to/hybrid-plugins/build) also specify schema 6; the local SDK README is the version-specific reference for this implementation.

### 1.2 Actual and added files

```text
C:\Users\Advaet\Documents\Projects\Gaslightr\
|-- CMakeLists.txt                         existing core targets + addon include
|-- cmake\
|   `-- UxpAddon.cmake                     complete native target definition
|-- include\
|   `-- nvngx_dlssnr_core.h                existing inferred runtime ABI
|-- src\                                 actual core folder (not src/core)
|   |-- DX12Context.h / .cpp
|   |-- NeuralBackend.h / .cpp
|   |-- MemoryBridge.h / .cpp
|   |-- PipelineSimulator.h / .cpp
|   |-- PixelContract.h
|   |-- TileProcessor.h / .cpp
|   |-- CUDAInterop.h / .cu               retained; detail mode disabled
|   |-- NodeBinding.cpp                   Node diagnostic module only
|   |-- PhotoshopMessaging.cpp            separate optional SDK companion
|   |-- main.cpp                          original standalone harness
|   `-- native\
|       `-- UxpAddonAdapter.cpp            new Adobe runtime adapter
|-- scripts\
|   `-- Stage-AdobeSdk.ps1                 local copy + SHA-256 verification
|-- vendor\                              ignored, local dependencies
|   |-- adobe\uxp\
|   |   |-- api\
|   |   |   |-- UxpAddonShared.h
|   |   |   `-- UxpAddonTypes.h
|   |   |-- utilities\
|   |   |   |-- UxpAddon.h / .cpp
|   |   |   |-- UxpTask.h / .cpp
|   |   |   `-- UxpValue.h / .cpp
|   |   |-- SDK-README.md
|   |   `-- Adobe-Developer-Additional-Terms_en-US_20220330.pdf
|   `-- node\                            diagnostic dependencies, not UXP
|-- uxp-frontend\                         junction must target THIS directory
|   |-- manifest.json                    schema 6, PS >=25.0, API v2
|   |-- index.html / index.jsx / styles.css
|   |-- bridge.js / pixels.js
|   |-- native-checks.js                  no-GPU boundary checks from panel
|   `-- win\x64\                         generated; ignored by Git
|       |-- gaslightr.uxpaddon             actual Adobe-native module
|       |-- gaslightr.pdb                  matching native debugging symbols
|       `-- nvngx_dlssnr.dll               separately supplied, verified runtime
|-- build\
|   |-- native\Release\
|   |   |-- gaslightr.uxpaddon / .pdb
|   |   `-- gaslightr.lib / .exp           import-library build artifacts
|   `-- Release\
|       |-- GaslightrCore.lib
|       |-- gaslightr_adobe_sdk.lib
|       `-- gaslightr.node                 optional; never load in Photoshop
|-- tests\
`-- docs\
    `-- OFFLINE_UXP_IMPLEMENTATION_GUIDE.md

C:\SDKs\Adobe\
|-- Photoshop\pluginsdk\                 native Photoshop SDK; unchanged
`-- UXP-Hybrid\
    |-- README.md
    |-- src\api\                         copy into vendor/adobe/uxp/api
    |-- src\utilities\                   copy preserving sibling api path
    `-- template\                        local reference, not a build dependency

%APPDATA%\Adobe\UXP\Plugins\External\
`-- Gaslightr\ [JUNCTION]
    -> C:\Users\Advaet\Documents\Projects\Gaslightr\uxp-frontend\

%APPDATA%\Adobe\UXP\logs\
`-- photoshop.log                        Photoshop-owned log, if emitted by host
```

Do not create an empty `src/core/` folder to make the tree look consistent. If you later move the core there, update all core source lists and include paths together in a separate change. The code below is for the actual current repository.

### 1.3 Architecture and ownership

```text
Panel action
  -> photoshop.core.executeAsModal(...)
  -> imaging.getPixels() for one bounded region
  -> chunky, linear sRGB RGBA Float32Array
  -> addon.processTile(view, options)       synchronous UXP scripting thread
  -> Gaslightr::ProcessTile(span, request)
  -> ValidateTile / MemoryBridge / PipelineSimulator / checked GPU fences
  -> new output Float32Array
  -> frontend blend + crop halo away
  -> imaging.putPixels() into one new raster layer
  -> resumeHistory(..., true), or rollback with false
```

`sdkMessaging` is **false** in this addon. The existing frontend therefore uses direct addon calls and does not wait for a separate `AutoPluginMain` companion. This path does not link `node.lib`, run Node, acquire Photoshop C++ suites, or register an `AutoPluginMain` entry point. The Photoshop C++ SDK is available for later host-suite work but is not required by this pure computation addon. All document reads and writes remain in UXP's modal Imaging API path.

The input view is borrowed, not copied, at the UXP/C++ boundary. Upload-heap normalization, GPU transfers, readback, and creation of the returned ArrayBuffer still involve copies. Native code never retains the pointer after returning. Shared backing memory is rejected. The synchronous method blocks the scripting thread during one tile, so cancellation is between tiles, not mid-inference.

## 2. Offline plugin registration workflow

### 2.1 Prepare Photoshop

1. Save work before restarting the host. Do not forcibly terminate Photoshop to unlock a DLL.
2. In Photoshop, open **Edit → Preferences → Plugins** and enable **Developer Mode**. If the host exposes **Show Developer Menu**, enable it as well; it is not a replacement for Developer Mode.
3. Exit Photoshop normally and verify its process has ended before compiling/staging. Native libraries can remain loaded even when their panel is closed.
4. Build and stage using section 4, then verify the junction below, and start Photoshop normally.
5. Look for **Plugins → Gaslightr → Gaslightr** or the host's equivalent entry for the `gaslightr` panel. A file found through a junction is not, by itself, proof that Photoshop registered the plugin.

No step connects to TCP 14001. Do not run `uxp-cli`, Chrome/CDP debugging, or a port-forwarding workaround; `No PS client found` is unrelated to this filesystem-and-native-debugger path.

### 2.2 Create or verify the junction safely

Run in PowerShell as the same Windows account that runs Photoshop. This script does not remove or replace existing plugin folders:

```powershell
$ErrorActionPreference = 'Stop'
$repo = 'C:\Users\Advaet\Documents\Projects\Gaslightr'
$frontend = Join-Path $repo 'uxp-frontend'
$external = Join-Path $env:APPDATA 'Adobe\UXP\Plugins\External'
$junction = Join-Path $external 'Gaslightr'

if (-not (Test-Path -LiteralPath (Join-Path $frontend 'manifest.json') -PathType Leaf)) {
    throw 'Frontend manifest is missing; do not link the repository root.'
}
New-Item -ItemType Directory -Path $external -Force | Out-Null
$existing = Get-Item -LiteralPath $junction -Force -ErrorAction SilentlyContinue
if ($null -eq $existing) {
    New-Item -ItemType Junction -Path $junction -Target $frontend | Out-Null
} else {
    if ($existing.LinkType -ne 'Junction') {
        throw "Existing path is not a junction; inspect it manually: $junction"
    }
    $actualTarget = [IO.Path]::GetFullPath([string]$existing.Target).TrimEnd('\')
    $expectedTarget = [IO.Path]::GetFullPath($frontend).TrimEnd('\')
    if ($actualTarget -ine $expectedTarget) {
        throw "Existing junction points elsewhere: $actualTarget"
    }
}
Get-Item -LiteralPath $junction -Force | Format-List FullName,LinkType,Target
Get-ChildItem -LiteralPath $junction -Force

$required = @('manifest.json', 'index.html', 'index.jsx', 'bridge.js',
    'pixels.js', 'native-checks.js', 'win\x64\gaslightr.uxpaddon')
foreach ($relative in $required) {
    $path = Join-Path $junction $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing through junction: $path" }
    Get-Item -LiteralPath $path | Select-Object FullName,Length,LastWriteTime
}
```

On this machine the existing `Gaslightr` junction already has the correct target. Leave unrelated junctions such as `SamplePlugin` alone. Do not use recursive deletion on an external-plugin directory or junction target.

### 2.3 Verify discovery separately from filesystem access

Restart Photoshop after changing the manifest or native binary. Check for the panel and the log markers described in section 5. If the panel does not appear, verify Developer Mode, schema 6, host ID `PS`, host version, duplicate manifest IDs, the addon filename, and the exact junction target.

**Host limitation:** an External directory is a local discovery location, but a junction cannot force a Photoshop build or managed installation to accept an unregistered plugin. Host policy, plugin registration metadata and version-specific loading behavior can still reject it. Do not edit Adobe installation databases by guesswork, claim success from `Test-Path`, or substitute another plugin's identity. If the host does not scan/accept the bundle, capture its local rejection evidence; that host-specific registration issue remains a blocker under the stated restrictions. This manual does not claim that discovery has already succeeded.

## 3. Complete source and build configuration

The repository now contains the files below. They are reproduced without omitted code so the manual can also be used to reconstruct this implementation.

### 3.1 `src/native/UxpAddonAdapter.cpp`

This source uses the installed SDK's actual macros. `capabilities` and `processTile` are **JavaScript properties**, registered by `uxp_addon_create_function`; they are not individual PE exports. The DLL exports are `uxp_addon_init` and `uxp_addon_terminate`.

The SDK lacks a typed-array-info call, so the adapter obtains `%TypedArray%.prototype` from an Adobe-created Float32Array and invokes its intrinsic accessors. This avoids accidentally accepting a view's shadowed `buffer`, `length` or `byteOffset` properties. It requires standard, unmodified built-in intrinsics in this plugin's JS environment; startup checks should run before installing libraries that monkey-patch them. It rejects a wrong brand, shared buffer, detached/empty view, misalignment, byte-range overflow, malformed dimensions and invalid pixels.

```cpp
#include "UxpAddon.h"
#include "TileProcessor.h"
#include "BackendCapabilities.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace Gaslightr::UXP {
namespace {
void Require(addon_status status, const char* operation) {
    if (status != addon_ok) throw std::runtime_error(operation);
}
addon_value Property(addon_env env, addon_value object, const char* name) {
    addon_value result = nullptr;
    Require(UxpAddonApis.uxp_addon_get_named_property(env, object, name, &result), name);
    return result;
}
addon_value String(addon_env env, const char* text) {
    addon_value result = nullptr;
    Require(UxpAddonApis.uxp_addon_create_string_utf8(env, text, std::strlen(text), &result), "Create string");
    return result;
}
size_t UnsignedInteger(addon_env env, addon_value value, size_t maximum, const char* name) {
    double number = 0;
    Require(UxpAddonApis.uxp_addon_get_value_double(env, value, &number), name);
    if (!std::isfinite(number) || number < 0 || number > static_cast<double>(maximum) || std::floor(number) != number)
        throw std::invalid_argument(std::string("Invalid ") + name);
    return static_cast<size_t>(number);
}
addon_value Fail(addon_env env, const char* message) noexcept {
    OutputDebugStringA("[Gaslightr] UXP adapter error: ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
    bool pending = false;
    // Preserve an existing JavaScript exception (including a failing getter).
    if (UxpAddonApis.uxp_addon_is_exception_pending(env, &pending) != addon_ok || !pending)
        UxpAddonApis.uxp_addon_throw_error(env, "GASLIGHTR_NATIVE", message);
    return nullptr;
}

// SDK 0.1.0 has no get_typedarray_info. Invoke the actual %TypedArray%
// accessors instead of reading shadowable input.buffer/length/byteOffset.
// All handles stay local, and all JavaScript calls finish BEFORE memory borrow.
struct TypedArrayAccessors {
    addon_env env;
    addon_value object = nullptr;
    addon_value descriptorFunction = nullptr;
    addon_value prototype = nullptr;
    addon_value global = nullptr;
    explicit TypedArrayAccessors(addon_env environment) : env(environment) {
        Require(UxpAddonApis.uxp_addon_get_global(env, &global), "Get global");
        object = Property(env, global, "Object");
        descriptorFunction = Property(env, object, "getOwnPropertyDescriptor");
        addon_value buffer = nullptr, sample = nullptr, floatPrototype = nullptr;
        void* unused = nullptr;
        Require(UxpAddonApis.uxp_addon_create_arraybuffer(env, sizeof(float), &unused, &buffer), "Create type probe buffer");
        Require(UxpAddonApis.uxp_addon_create_typedarray(env, addon_float32_array, 1, buffer, 0, &sample), "Create type probe");
        Require(UxpAddonApis.uxp_addon_get_prototype(env, sample, &floatPrototype), "Get Float32Array prototype");
        Require(UxpAddonApis.uxp_addon_get_prototype(env, floatPrototype, &prototype), "Get TypedArray prototype");
    }
    addon_value Get(addon_value input, addon_value key) const {
        addon_value descriptor = nullptr, result = nullptr;
        const addon_value args[] = { prototype, key };
        Require(UxpAddonApis.uxp_addon_call_function(env, object, descriptorFunction, 2, args, &descriptor), "Get intrinsic descriptor");
        const addon_value getter = Property(env, descriptor, "get");
        Require(UxpAddonApis.uxp_addon_call_function(env, input, getter, 0, nullptr, &result), "Read intrinsic TypedArray metadata");
        return result;
    }
    addon_value Get(addon_value input, const char* name) const { return Get(input, String(env, name)); }
    void RequireFloat32(addon_value input) const {
        bool typed = false;
        Require(UxpAddonApis.uxp_addon_is_typedarray(env, input, &typed), "Check TypedArray");
        if (!typed) throw std::invalid_argument("Expected a Float32Array.");
        const addon_value symbol = Property(env, global, "Symbol");
        const addon_value tag = Get(input, Property(env, symbol, "toStringTag"));
        char name[32]{};
        size_t length = 0;
        Require(UxpAddonApis.uxp_addon_get_value_string_utf8(env, tag, name, sizeof(name), &length), "Read TypedArray brand");
        if (length != 12 || std::strcmp(name, "Float32Array") != 0)
            throw std::invalid_argument("Expected a Float32Array, not another typed-array type.");
    }
};
}

addon_value ProcessTile(addon_env env, addon_callback_info info) noexcept {
    try {
        size_t argc = 3;
        addon_value args[3]{};
        Require(UxpAddonApis.uxp_addon_get_cb_info(env, info, &argc, args, nullptr, nullptr), "Read arguments");
        if (argc != 2) throw std::invalid_argument("processTile requires pixels and options.");
        TileRequest request;
        // Unbox options before retrieving the pixel pointer. Option getters can
        // run JavaScript and must never run while a borrowed pointer is in use.
        request.width = static_cast<uint32_t>(UnsignedInteger(env, Property(env, args[1], "width"), 2048, "width"));
        request.height = static_cast<uint32_t>(UnsignedInteger(env, Property(env, args[1], "height"), 2048, "height"));
        request.preset = static_cast<int>(UnsignedInteger(env, Property(env, args[1], "preset"), 2, "preset"));
        Require(UxpAddonApis.uxp_addon_get_value_bool(env, Property(env, args[1], "detailOnly"), &request.detailOnly), "Read detailOnly");
        if (!request.width || !request.height) throw std::invalid_argument("Tile dimensions must be 1-2048.");
        const size_t expected = static_cast<size_t>(request.width) * request.height * 4;

        TypedArrayAccessors accessors(env);
        accessors.RequireFloat32(args[0]);
        const addon_value backing = accessors.Get(args[0], "buffer");
        const size_t offset = UnsignedInteger(env, accessors.Get(args[0], "byteOffset"), 9007199254740991ULL, "byteOffset");
        const size_t count = UnsignedInteger(env, accessors.Get(args[0], "length"), expected, "length");
        if (count != expected) throw std::invalid_argument("Expected exactly width * height * 4 components.");
        bool ordinary = false;
        Require(UxpAddonApis.uxp_addon_is_arraybuffer(env, backing, &ordinary), "Check ArrayBuffer");
        if (!ordinary) throw std::invalid_argument("SharedArrayBuffer input is unsupported.");
        void* base = nullptr;
        size_t bytes = 0;
        Require(UxpAddonApis.uxp_addon_get_arraybuffer_info(env, backing, &base, &bytes), "Borrow ArrayBuffer");
        if (!base || offset % alignof(float) != 0 || offset > bytes || count > (bytes - offset) / sizeof(float))
            throw std::invalid_argument("Detached, unaligned or out-of-bounds input view.");
        const auto* input = reinterpret_cast<const float*>(static_cast<const unsigned char*>(base) + offset);
        // No JS calls or callbacks between here and the synchronous core return.
        // The local handles root the view. The core validates finite half-float
        // range/alpha, serializes NGX access and checks WaitForGPU before return.
        ValidateTile({input, count}, request);
        OutputDebugStringA("[Gaslightr] processTile begin\n");
        const auto result = Gaslightr::ProcessTile({input, count}, request);
        if (result.size() != expected) throw std::runtime_error("Unexpected native output length.");
        OutputDebugStringA("[Gaslightr] processTile GPU work drained\n");
        addon_value buffer = nullptr, output = nullptr;
        void* destination = nullptr;
        Require(UxpAddonApis.uxp_addon_create_arraybuffer(env, result.size() * sizeof(float), &destination, &buffer), "Allocate output buffer");
        if (!destination) throw std::runtime_error("Cannot allocate output memory.");
        std::memcpy(destination, result.data(), result.size() * sizeof(float));
        Require(UxpAddonApis.uxp_addon_create_typedarray(env, addon_float32_array, result.size(), buffer, 0, &output), "Wrap output Float32Array");
        return output;
    } catch (const std::exception& error) { return Fail(env, error.what()); }
    catch (...) { return Fail(env, "Unknown native tile-processing failure."); }
}

addon_value Capabilities(addon_env env, addon_callback_info) noexcept {
    try {
        addon_value result = nullptr, value = nullptr;
        Require(UxpAddonApis.uxp_addon_create_object(env, &result), "Create capabilities");
        Require(UxpAddonApis.uxp_addon_create_int32(env, 1, &value), "Create protocol");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "protocol", value), "Set protocol");
        Require(UxpAddonApis.uxp_addon_get_boolean(env, false, &value), "Create capability flag");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "detailOnly", value), "Set detailOnly");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "sdkMessaging", value), "Set sdkMessaging");
        Require(UxpAddonApis.uxp_addon_get_boolean(env, kNeuralInferenceAvailable, &value), "Create inference flag");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "neuralInference", value), "Set neuralInference");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "neuralInferenceReason",
            String(env, kNeuralInferenceReason)), "Set neuralInferenceReason");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "executionMode",
            String(env, kExecutionMode)), "Set executionMode");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "detailOnlyReason",
            String(env, "CUDA texture interop requires repair and device validation.")), "Set detailOnlyReason");
        return result;
    } catch (const std::exception& error) { return Fail(env, error.what()); }
    catch (...) { return Fail(env, "Cannot query capabilities."); }
}

addon_value Init(addon_env env, addon_value exports, const addon_apis&) noexcept {
    try {
        const struct { const char* name; addon_callback callback; } functions[] = {
            {"capabilities", Capabilities}, {"processTile", ProcessTile}
        };
        for (const auto& function : functions) {
            addon_value wrapped = nullptr;
            Require(UxpAddonApis.uxp_addon_create_function(env, function.name, std::strlen(function.name), function.callback, nullptr, &wrapped), "Register function");
            Require(UxpAddonApis.uxp_addon_set_named_property(env, exports, function.name, wrapped), "Export function");
        }
        OutputDebugStringA("[Gaslightr] UXP addon initialized\n");
        return exports;
    } catch (const std::exception& error) { return Fail(env, error.what()); }
    catch (...) { return Fail(env, "Cannot initialize Gaslightr addon."); }
}
void Terminate(addon_env) noexcept {
    // No persistent JS handles, worker tasks or GPU queues belong to this adapter.
    // Each synchronous core call has already drained and destroyed its resources.
    OutputDebugStringA("[Gaslightr] UXP addon terminated\n");
}
}
UXP_ADDON_INIT(Gaslightr::UXP::Init)
UXP_ADDON_TERMINATE(Gaslightr::UXP::Terminate)
```

**Fence ownership:** `TileProcessor` owns the D3D12 context. Its `Submit()` closes and executes the command list and checks `WaitForGPU()`. The simulator checks completion per frame; readback is fenced; the processor checks another fence before return; its cleanup guards drain before releasing resources. The adapter calls that synchronous function and allocates the returned JS buffer only afterward. Creating an unrelated D3D12 context and waiting on its fence in the adapter would not synchronize the actual queue and would be incorrect.

On a device-removal/fence failure the call throws rather than returning pixel data. A permanently hung driver can still block this synchronous design: no artificial timeout frees in-flight resources. Production timeout/recovery design and runtime ABI validation remain separate work.

### 3.2 `cmake/UxpAddon.cmake`

```cmake
option(GASLIGHTR_BUILD_UXP_ADDON "Build Adobe's native UXP addon" OFF)
if(GASLIGHTR_BUILD_UXP_ADDON)
    set(_adobe "${PROJECT_SOURCE_DIR}/vendor/adobe/uxp")
    set(_adobe_sources
        "${_adobe}/utilities/UxpAddon.cpp"
        "${_adobe}/utilities/UxpTask.cpp"
        "${_adobe}/utilities/UxpValue.cpp")
    foreach(_file IN LISTS _adobe_sources)
        if(NOT EXISTS "${_file}")
            message(FATAL_ERROR "Missing ${_file}. Run scripts/Stage-AdobeSdk.ps1 first.")
        endif()
    endforeach()
    # Isolate SDK warnings from our /WX policy; PCH is disabled for EVERY unit.
    add_library(gaslightr_adobe_sdk STATIC ${_adobe_sources})
    set_target_properties(gaslightr_adobe_sdk PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        DISABLE_PRECOMPILE_HEADERS ON)
    set_source_files_properties(${_adobe_sources} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(gaslightr_adobe_sdk SYSTEM PUBLIC
        "${_adobe}/api" "${_adobe}/utilities")
    target_compile_definitions(gaslightr_adobe_sdk PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(gaslightr_adobe_sdk PRIVATE /Y- /Z7 /EHsc /utf-8 /W3)

    add_library(gaslightr_uxpaddon SHARED "${PROJECT_SOURCE_DIR}/src/native/UxpAddonAdapter.cpp")
    gaslightr_compile(gaslightr_uxpaddon)
    target_compile_options(gaslightr_uxpaddon PRIVATE /Y- /Z7)
    target_link_options(gaslightr_uxpaddon PRIVATE /DEBUG:FULL /INCREMENTAL:NO)
    target_link_libraries(gaslightr_uxpaddon PRIVATE GaslightrCore gaslightr_adobe_sdk d3d12.lib dxgi.lib)
    set_target_properties(gaslightr_uxpaddon PROPERTIES
        PREFIX "" OUTPUT_NAME "gaslightr" SUFFIX ".uxpaddon"
        PDB_NAME "gaslightr"
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        DISABLE_PRECOMPILE_HEADERS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native/$<CONFIG>")
    # PDBs for core breakpoints in Release without changing Release's /MT runtime.
    target_compile_options(GaslightrCore PRIVATE /Z7)
    set(_stage "${PROJECT_SOURCE_DIR}/uxp-frontend/win/x64")
    add_custom_command(TARGET gaslightr_uxpaddon POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_stage}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:gaslightr_uxpaddon>" "${_stage}/gaslightr.uxpaddon"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_PDB_FILE:gaslightr_uxpaddon>" "${_stage}/gaslightr.pdb"
        COMMENT "Staging native addon and matching symbols into the UXP bundle"
        VERBATIM)
endif()
```

Add this single line to the root `CMakeLists.txt` **after** `GaslightrCore` and the `gaslightr_compile` helper exist. It is already present in this repository:

```cmake
include(cmake/UxpAddon.cmake)
```

The existing `GaslightrCore` target supplies `src/` and `include/` through the compilation helper and links `d3d12`, `dxgi`, `dxguid` and `Advapi32`. Do not replace the entire root CMake file or remove its test targets.

Important build details:

- **C1010:** both the addon and every SDK compilation unit get `/Y-`; `DISABLE_PRECOMPILE_HEADERS` and `SKIP_PRECOMPILE_HEADERS` are also set. Never edit Adobe helpers to include an application-specific `pch.h`.
- **Static CRT:** Release uses `/MT`; Debug uses `/MTd`. Compile the core, adapter and helper library in the same configuration. Do not mix a Debug core with a Release addon.
- **Symbols:** `/Z7` embeds compiler debug data in object files; `/DEBUG:FULL` emits the final link PDB. This avoids the compiler PDB manager dependency that produced `C1902` during bring-up. Release optimization remains enabled, so some locals/lines may be optimized away.
- **Output collision:** import libraries, exports, binary and PDB for UXP live under `build/native/<config>`, avoiding the diagnostic Node module's `build/Release/gaslightr.lib`.
- **SDK helpers:** `/WX` remains enabled for project code; the unmodified Adobe helpers compile as a separate target with their own warning policy. `UxpTask.cpp` and `UxpValue.cpp` are compiled even though the synchronous adapter does not use their task/value wrappers.
- **No fake exports:** the SDK header declares its C-linkage exports. No `.def` file, Node registration macro, PiPL resource or Photoshop C++ import library is needed for this direct addon.

### 3.3 Frontend manifest

Keep Photoshop 25.0 as the minimum, but use the installed hybrid SDK's schema 6:

```json
{
  "manifestVersion": 6,
  "id": "com.advaet.gaslightr",
  "name": "Gaslightr",
  "version": "0.2.0",
  "main": "index.html",
  "addon": { "name": "gaslightr.uxpaddon" },
  "host": { "app": "PS", "minVersion": "25.0.0", "data": { "apiVersion": 2 } },
  "requiredPermissions": { "enableAddon": true, "localFileSystem": "plugin" },
  "entrypoints": [{
    "type": "panel", "id": "gaslightr", "label": { "default": "Gaslightr" },
    "minimumSize": { "width": 300, "height": 580 },
    "preferredDockedSize": { "width": 340, "height": 720 },
    "preferredFloatingSize": { "width": 380, "height": 760 }
  }]
}
```

The existing loader remains:

```javascript
const addon = await require("gaslightr.uxpaddon");
```

Call this inside an async function, as in the installed SDK sample. Await addon loading before inspecting its functions or calling `capabilities`; checking the unresolved Promise incorrectly reports an incompatible addon. Use the SDK's platform-aware addon resolution and the `win/x64` bundle layout. Do not load `build/Release/gaslightr.node`, do not rename it, and do not pass pixel arrays through JSON messaging.

### 3.4 Panel diagnostics and logging

The frontend now includes **Check native bridge**. It invokes native validation from a modal scope and checks six deliberately invalid inputs without loading the neural DLL or submitting GPU work. One case uses a nonzero-offset Float32Array whose surrounding storage contains NaNs; passing this case verifies the adapter reads the intended view before rejecting its deliberately invalid alpha.

`index.jsx` emits prefixed `console.log` messages for script load, capabilities and panel status. Native entry points use `OutputDebugStringA`, visible in Visual Studio's Debug Output while attached. This is separate from Photoshop's JS log: native debug output is not automatically written to `photoshop.log`.

## 4. Step-by-step compilation and staging procedure

### 4.1 Open the right shell and verify prerequisites

Open **Developer PowerShell for VS 2022**, preferably x64. No administrator rights should be necessary for building in the repository.

```powershell
$ErrorActionPreference = 'Stop'
Set-Location 'C:\Users\Advaet\Documents\Projects\Gaslightr'
Get-Command cmake,cl,dumpbin | Select-Object Name,Source
cmake --version
if ($LASTEXITCODE -ne 0) { throw 'CMake is unavailable' }

$requiredSdkFiles = @(
    'C:\SDKs\Adobe\UXP-Hybrid\src\api\UxpAddonShared.h',
    'C:\SDKs\Adobe\UXP-Hybrid\src\api\UxpAddonTypes.h',
    'C:\SDKs\Adobe\UXP-Hybrid\src\utilities\UxpAddon.h',
    'C:\SDKs\Adobe\UXP-Hybrid\src\utilities\UxpAddon.cpp',
    'C:\SDKs\Adobe\UXP-Hybrid\src\utilities\UxpTask.cpp',
    'C:\SDKs\Adobe\UXP-Hybrid\src\utilities\UxpValue.cpp',
    'C:\SDKs\Adobe\Photoshop\pluginsdk\photoshopapi\photoshop\PIUXPSuite.h'
)
foreach ($file in $requiredSdkFiles) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing SDK file: $file" }
}
Get-Process Photoshop -ErrorAction SilentlyContinue | Select-Object Id,Path
```

If `cl` or `dumpbin` is missing, use the Visual Studio Developer PowerShell shortcut rather than adding arbitrary compiler directories to the system PATH. The local compiler used here is MSVC 19.44 / v143 with Windows SDK 10.0.26100. Do not install or download anything as part of this offline build.

If the final command lists Photoshop, save and exit it normally before continuing. The currently installed Photoshop executable is `C:\Program Files\Adobe\Adobe Photoshop 2024\Photoshop.exe`; its file version reports **25.0**.

### 4.2 Copy the SDK helpers locally

Run the provided script:

```powershell
& .\scripts\Stage-AdobeSdk.ps1 -SdkRoot 'C:\SDKs\Adobe\UXP-Hybrid'
Get-ChildItem .\vendor\adobe\uxp -Recurse -File | Select-Object FullName,Length
```

Its full implementation, including exact copy paths and verification, is:

```powershell
[CmdletBinding()]
param([string]$SdkRoot = 'C:\SDKs\Adobe\UXP-Hybrid')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $repo 'vendor\adobe\uxp'
$files = @(
    'src\api\UxpAddonShared.h', 'src\api\UxpAddonTypes.h',
    'src\utilities\UxpAddon.h', 'src\utilities\UxpAddon.cpp',
    'src\utilities\UxpTask.h', 'src\utilities\UxpTask.cpp',
    'src\utilities\UxpValue.h', 'src\utilities\UxpValue.cpp'
)
foreach ($relative in $files) {
    $source = Join-Path $SdkRoot $relative
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing SDK file: $source" }
}
foreach ($relative in $files) {
    $source = Join-Path $SdkRoot $relative
    $target = Join-Path $destination $relative.Substring(4)
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $target -Force
    if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $target).Hash) { throw "Copy verification failed: $target" }
}
Copy-Item -LiteralPath (Join-Path $SdkRoot 'README.md') -Destination (Join-Path $destination 'SDK-README.md') -Force
Get-ChildItem -LiteralPath $SdkRoot -Filter '*.pdf' | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
}
Write-Output "Staged verified Adobe SDK files in $destination"
```

The SDK headers include sibling directories using `../api/...`, so preserve `api/` and `utilities/` under the same parent. The original `C:\SDKs` installation is read-only for this workflow. The copied SDK and its license remain in ignored `vendor/`; respect Adobe's license before distributing any part of the SDK.

### 4.3 Configure and compile Release x64

```powershell
Set-Location 'C:\Users\Advaet\Documents\Projects\Gaslightr'
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 `
    -DGASLIGHTR_BUILD_UXP_ADDON=ON `
    -DGASLIGHTR_BUILD_NODE_BINDING=OFF `
    -DGASLIGHTR_CHECK_PHOTOSHOP_SDK=OFF `
    -DGASLIGHTR_BUILD_HARNESS=OFF `
    -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }

cmake --build build --config Release --target gaslightr_uxpaddon GaslightrContractTests
if ($LASTEXITCODE -ne 0) { throw 'Build failed; do not test an old staged binary' }

ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'C++ contract tests failed' }
& 'C:\Program Files\nodejs\node.exe' --test tests/frontend.test.js
if ($LASTEXITCODE -ne 0) { throw 'Frontend offline tests failed' }
```

Node is used here only as a local offline test runner. Photoshop never loads the Node binding. No `npm install`, package manager, registry service, or network connection is involved.

If `build/` was configured for another generator/architecture, use a new local build directory such as `build/uxp-vs2022` for all three CMake/CTest commands; do not delete unrelated build outputs. The post-build staging destination remains the same.

### 4.4 Verify binary, architecture, exports, symbols and copies

```powershell
$repo = 'C:\Users\Advaet\Documents\Projects\Gaslightr'
$built = Join-Path $repo 'build\native\Release\gaslightr.uxpaddon'
$staged = Join-Path $repo 'uxp-frontend\win\x64\gaslightr.uxpaddon'
$symbols = Join-Path $repo 'uxp-frontend\win\x64\gaslightr.pdb'
foreach ($file in @($built, $staged, $symbols)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing output: $file" }
    $item = Get-Item -LiteralPath $file
    if ($item.Length -eq 0) { throw "Empty output: $file" }
    $item | Select-Object FullName,Length,LastWriteTime
}
if ((Get-FileHash -LiteralPath $built).Hash -ne (Get-FileHash -LiteralPath $staged).Hash) {
    throw 'The staged addon does not match the successful build'
}
dumpbin /headers $staged | Select-String 'machine|DLL'
dumpbin /exports $staged
dumpbin /dependents $staged
Select-String -Path "$repo\build\gaslightr_adobe_sdk.vcxproj" -Pattern '/Y-|PrecompiledHeader|RuntimeLibrary'
Select-String -Path "$repo\build\gaslightr_uxpaddon.vcxproj" -Pattern '/Y-|RuntimeLibrary|DebugInformationFormat'
```

Expect x64/`8664`, DLL characteristics, and **exactly the two intended Adobe entry points** `uxp_addon_init` and `uxp_addon_terminate`. Export RVAs are build-specific; do not hardcode them. The initial verified build here was 596,480 bytes, with a 7,884,800-byte PDB; future valid builds need not have those sizes.

`capabilities` and `processTile` do not appear in `dumpbin /exports`: the initialization function registers them in JavaScript. The DLL should not import `node.exe` or `node.dll`. The neural runtime is dynamically loaded, so it will not appear as a normal static import in `/dependents`. `/MT` removes a dynamic CRT dependency, not dependencies on Windows system DLLs or on the separately loaded neural runtime.

For the Visual Studio generator, CMake translates `/Y-` into `<PrecompiledHeader>NotUsing</PrecompiledHeader>` in the generated project. Seeing that setting on the helper target and source items is expected even when the literal `/Y-` is absent from the project text. Release's runtime setting is `MultiThreaded`; Debug's is `MultiThreadedDebug`.

Always keep the exact matching PDB. Copying yesterday's PDB next to today's DLL does not make debugging work.

### 4.5 Stage the neural runtime only after ABI validation

The native bridge can be loaded and its boundary checks run without the neural DLL. Actual rendering requires a verified `nvngx_dlssnr.dll` beside the addon:

```powershell
$runtime = 'C:\Users\Advaet\Documents\Projects\Gaslightr\uxp-frontend\win\x64\nvngx_dlssnr.dll'
Test-Path -LiteralPath $runtime -PathType Leaf
if (Test-Path -LiteralPath $runtime -PathType Leaf) {
    Get-Item -LiteralPath $runtime | Select-Object FullName,Length,VersionInfo
    Get-FileHash -LiteralPath $runtime -Algorithm SHA256
    dumpbin /exports $runtime
    dumpbin /dependents $runtime
}
```

The repository looks for `NVSDK_NGX_DLSSNR_InitializeNR` and `NVSDK_NGX_DLSSNR_EvaluateNR`, with the existing alternate names `NVSDK_NGX_InitializeNR` and `NVSDK_NGX_EvaluateNR`. Matching names alone do not establish ABI compatibility. The reference structs are inferred; obtain authoritative signatures, resource-state and lifetime rules, and a shutdown/feature-destruction contract before testing an unknown binary inside Photoshop. No substitute neural algorithm or success response is fabricated if the runtime is missing.

The original registry-writing diagnostic harness is disabled in the commands above. Neither this setup nor the addon adapter changes firewall rules, driver security settings or machine registry keys.

## 5. Offline verification and native debugging SOP

### 5.1 Start by checking host logs

Open a second PowerShell window and run:

```powershell
$log = Join-Path $env:APPDATA 'Adobe\UXP\logs\photoshop.log'
Test-Path -LiteralPath $log -PathType Leaf
if (Test-Path -LiteralPath $log -PathType Leaf) {
    Get-Item -LiteralPath $log | Select-Object FullName,Length,LastWriteTime
    Get-Content -LiteralPath $log -Tail 150 -Wait |
        Select-String -Pattern 'Gaslightr|com\.advaet\.gaslightr|addon|manifest|Error|Exception'
} else {
    Write-Warning "Photoshop has not created the expected log: $log"
    Get-ChildItem -LiteralPath (Join-Path $env:APPDATA 'Adobe\UXP') `
        -Recurse -File -Filter '*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 20 FullName,Length,LastWriteTime
}
```

`-Wait` keeps the command running; press **Ctrl+C** in that PowerShell window to stop monitoring, not to cancel Photoshop. After log rotation or a host restart, restart the command if new lines stop appearing. For unfiltered context use `Get-Content -LiteralPath $log -Tail 300`.

Open the panel and look for `[Gaslightr] panel script loaded`, native capability output, and status/error markers. **Verify the logging route:** host builds differ in how they persist `console.log`. A missing line is not proof the callback did not execute. Do not create an empty `photoshop.log` or write native messages into Adobe's log to make monitoring appear successful. If this host does not create that file or persist JS console output there, the exact requested log route remains unverified; use the visible panel status and Visual Studio native output while diagnosing it. All those diagnostics remain offline.

### 5.2 Confirm the bridge without invoking the neural runtime

1. Start Photoshop and open the Gaslightr panel.
2. Click **Check native bridge**. No document is required; the operation is modal for consistency.
3. Expected status: **6 native boundary checks passed; GPU inference was not run.**
4. Detail-Only must remain disabled with its explanation. `protocol` is 1 and `sdkMessaging` is false.
5. If the addon cannot load, diagnose the manifest, architecture and dependency error before testing rendering. A failed boundary check is not a reason to skip validation.

This host-run check is especially important because SDK 0.1.0 exposes only part of the Node-like API. Compilation proves that the calls exist in the SDK table; this check verifies the runtime implements the metadata and exception behavior used by the adapter. It has not been run in Photoshop during preparation of this manual.

### 5.3 Attach Visual Studio 2022 to the native host process

1. Start Photoshop normally under the same Windows account as Visual Studio. Avoid elevating either program unless your installation requires it; privilege mismatches can prevent attachment.
2. Open `C:\Users\Advaet\Documents\Projects\Gaslightr\build\Gaslightr.sln` in Visual Studio. Select **Release | x64** for the built binary, or open the source files in an existing solution.
3. Choose **Debug → Attach to Process**. Select the local `Photoshop.exe`. If several instances exist, use the PID and image path shown by `Get-Process Photoshop | Select-Object Id,Path` to choose the correct one.
4. Under **Attach to / Code Type**, select **Native**. Do not select JavaScript, a browser connection or remote transport. Click **Attach**.
5. Open **Debug → Windows → Modules** after the panel loads. Locate `gaslightr.uxpaddon`; verify the module path is the staged `win\x64` binary (the display may resolve the junction to its target).
6. Use **Load Symbols** if needed and select `uxp-frontend\win\x64\gaslightr.pdb`. The Modules window must report symbols loaded. Inspect **Symbol Load Information** if the breakpoint remains hollow.
7. Add a function breakpoint on **`Gaslightr::UXP::ProcessTile`** or a source breakpoint at `ValidateTile({input, count}, request)`. Also useful: `Gaslightr::ProcessTile`, `MemoryBridge::UploadToGPU`, `PipelineSimulator::ExecuteFullPipeline`, and `DX12Context::WaitForGPU`.
8. Click **Check native bridge** to prove native breakpoint entry without needing a neural DLL. To step GPU execution, use Preview only after the runtime validation gate.
9. Inspect `request.width`, `request.height`, `request.preset`, `request.detailOnly`, `offset`, `count` and `bytes`. The input pointer must be `base + offset`, with `count == width * height * 4`. Use **Debug → Windows → Memory → Memory 1** to inspect `input`; display four-byte floating values if the debugger provides that format. The pointer is valid only within the synchronous callback.
10. Watch the **Output → Debug** pane for native `[Gaslightr]` messages. F10/F11 step through validation and the processor; F5 continues execution.

Release is built with symbols but optimized. For clearer stepping, build and stage Debug using `cmake --build build --config Debug --target gaslightr_uxpaddon`, then use **Debug | x64** and the newly staged matching PDB. Debug uses `/MTd`; keep all native targets on that configuration. The core requests the D3D12 debug layer in Debug, so use it only if the local graphics debug components are installed, or diagnose their absence separately. Return to Release for deployment.

To rebuild, save documents, detach the debugger, and exit Photoshop normally. A closed panel is not a reliable native-unload boundary. Rebuild, verify hashes, restart Photoshop, and reattach. Never overwrite a loaded binary and assume the new code is running.

### 5.4 Exceptions, device failures and crash capture

In **Debug → Windows → Exception Settings**, enable first-chance breaks for C++ exceptions while diagnosing native validation. Expected negative checks deliberately throw; continue after inspecting them. Enable access-violation breaks for suspected DLL/ABI faults. The existing backend catches some structured exceptions, so a first-chance debugger break can show the originating fault before that handler translates it.

For an unexpected crash or serious native fault:

1. Stop at the first meaningful exception rather than continuing blindly.
2. Record the exception code, call stack, module list, runtime DLL hash, dimensions/preset and exact staged addon/PDB.
3. Use **Debug → Save Dump As** while attached and paused, choosing a dump with heap if memory inspection is needed. Save under a local diagnostics directory and keep the matching binaries/symbols.
4. Reopen the dump in Visual Studio with native debugging and local symbols. No symbol server or remote debugger is required for project frames.
5. Treat `DXGI_ERROR_DEVICE_REMOVED`, GPU fence errors, missing exports and ABI exceptions as failed rendering. Do not return a partially filled output array to Photoshop.

A heap dump may contain document pixels and other process memory. Keep it local unless you deliberately choose to share it.

### 5.5 In-panel Photoshop acceptance test

**Prerequisite:** native boundary checks pass and the actual neural runtime ABI has been independently validated. If either prerequisite fails, stop at diagnostics; a compiled adapter alone does not establish neural-rendering success.

1. Create a disposable document with **File → New**, width **1024 pixels**, height **1024 pixels**, RGB color, **32 bits/channel**, and an sRGB profile. If the New Document dialog does not offer the bit depth, create RGB and use **Image → Mode → 32 Bits/Channel**. Verify the document is actually 32-bit before continuing.
2. Create/select one normal raster pixel layer. Make it cover the full canvas; an opaque fill avoids Photoshop trimming away transparent margins. Include a smooth gradient, a fine diagonal line, noise/fine texture and recognizable colored patches crossing x=960 and y=960. Do not use a Smart Object, adjustment layer or layer group for the first test.
3. Record the original layer count/name, document profile, bit depth and bounds. Set NR Intensity to **0** and Local Tone / Structure and Mask Feathering to **0**, with Default style and Quality preset. Even at intensity 0 the current pipeline still invokes the neural processor; it is not a runtime-free test.
4. Click **Preview**. The current preview is a full-resolution center crop, not the full Photoshop viewport. Drag across it to compare. No new document layer or undo entry should be created by the preview itself.
5. Click **Enhance layer**. Expect one additional raster layer named `<source name> · Gaslightr`, with original alpha retained. Inspect the layer name, raster type, bounds and dimensions in the Layers/Properties panels. The source layer, document profile and bit depth must remain intact. Existing masks/effects/blend modes are not duplicated by this raster-output path.
6. At intensity 0, compare output against the original; full-image frontend blending should restore the original pixel values before Photoshop profile/writeback processing. Any observed difference needs investigation of host color conversion and writeback; do not dismiss it as neural styling.
7. Open **Window → History**. The entire apply should be a single **Gaslightr enhancement** history state. Press **Ctrl+Z** once: the entire enhanced layer must disappear. Redo and confirm it returns as one operation.
8. Repeat with intensity 0.5. Inspect the image at **100% and 200%** zoom, including the seam locations below. Alpha should not become opaque at partially transparent edges. Repeat later with 8-/16-bit input, non-sRGB profiles, HDR highlights and negative layer offsets.
9. On a larger disposable canvas, request cancellation between tiles. The partially created output layer must be removed by history rollback. The synchronous native call may defer cancellation until the current tile finishes. A missing or invalid runtime should also roll back any output created before the error.

### 5.6 Inspect the actual halo geometry

For a full 1024×1024 layer with bounds `(0,0)-(1024,1024)`, the 960-pixel step produces **four tiles**, not one:

| Tile | Written core, half-open coordinates | Read region including clipped halo | Read size |
| --- | --- | --- | --- |
| Top-left | `[0,960) × [0,960)` | `[0,992) × [0,992)` | 992×992 |
| Top-right | `[960,1024) × [0,960)` | `[928,1024) × [0,992)` | 96×992 |
| Bottom-left | `[0,960) × [960,1024)` | `[0,992) × [928,1024)` | 992×96 |
| Bottom-right | `[960,1024) × [960,1024)` | `[928,1024) × [928,1024)` | 96×96 |

The halo extends **32 pixels per side** where neighboring pixels exist. Adjacent read regions overlap by **64 pixels total**, from 928 through 991 around a seam at 960. Written cores never overlap. Inspect x=960, y=960 and their intersection; use guides/rulers positioned at those pixel coordinates if helpful. A 2048×2048 canvas additionally exercises seams at 1920 and full 1024-pixel interior reads.

The installed neural runtime may reject very small edge tiles or require a larger receptive field. The adapter's allowed 1–2048 size range is a memory contract, not evidence every such size is supported by the model. If the 96-pixel edge tile fails, implement tested padding/cropping consistent with the real runtime before claiming full-image support. Halo overlap and passing offline filter tests do not guarantee seam-free neural inference; independently reset tile histories can still produce discontinuities.

### 5.7 Troubleshooting decision table

| Symptom | Local diagnosis and next action |
| --- | --- |
| `C1010` in an Adobe helper | Verify `/Y-` and disabled PCH on the **helper target**, not only on the adapter. Reconfigure CMake; build its generated solution instead of an old hand-created VS project. |
| `C1902` compiler PDB manager mismatch | Confirm `/Z7`; the provided target embeds debug data in objects and produces the final PDB during linking. Use one coherent VS toolchain. |
| `LNK1114` / import-library collision | Confirm UXP's archive output is `build/native/<config>`, separate from Node. |
| Post-build copy denied | Photoshop or a debugger may still hold the staged module. Save, detach and exit normally, then rebuild. |
| Plugin missing from menu | Verify junction, Developer Mode, schema 6, host version, manifest ID and logs. Filesystem visibility alone does not prove host registration. |
| `Cannot find module gaslightr.uxpaddon` | Verify addon declaration, exact filename and `win/x64` placement through the junction; restart the host. |
| Windows says module not found although addon exists | Inspect `/dependents` and loader diagnostics for a missing dependent DLL; do not assume the named addon file is the missing dependency. |
| Empty/missing `photoshop.log` | Verify the actual host logging path/routing locally. Use panel diagnostics and native Debug Output; do not synthesize a host log. |
| `No PS client found` | A remote-debugging tool is still being used. It is not part of this workflow; use local native Attach to Process. |
| Native checks fail on metadata access | Inspect exact SDK/runtime support for typed-array intrinsics and API table; do not invent a typed-array-info function or bypass validation. |
| `nvngx_dlssnr.dll is missing` | Bridge load has succeeded far enough to reach core preflight. Supply a verified runtime beside the addon before rendering. |
| Export resolution/SEH failure | Runtime ABI remains incompatible or unverified. Stop rendering and inspect in a standalone/native debug process. |
| Detail-Only unavailable | Expected: CUDA interop repair and device validation are pending. Do not turn the capability flag on to hide the error. |
| GPU success but visible tile boundaries | Investigate model context/edge dimensions/history behavior. The 32-pixel halo only establishes the current local-filter support. |

### 5.8 Completion criteria

The native bridge is ready for host integration when the build, export check and in-panel boundary checks pass. Full rendering is ready for broader testing only when the actual runtime contract, pixel/alpha/profile behavior, single-history rollback, edge tiles, repeated-load memory usage and neural seams have also passed. Neither the offline tests nor this guide establish those runtime outcomes in advance.

Local source references: `C:\SDKs\Adobe\UXP-Hybrid\README.md`, `src\api\UxpAddonShared.h`, `src\utilities\UxpAddon.h` and the SDK template `module.cpp`. Web references are optional reading only; no build or debugging step depends on network access.
