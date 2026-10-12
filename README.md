# Gaslightr

Experimental Windows x64 Photoshop enhancement project using a headless D3D12 temporal pipeline.

The UXP frontend, bounded Imaging API orchestration, C++ tile processor, and diagnostic Node-API binding are implemented. **This is not yet an installable, end-to-end Photoshop plugin.** Adobe SDK integration, native addon packaging, and the inferred neural-runtime ABI still require validation. Detail-Only mode is disabled pending CUDA interop repair.

Start with [SDK setup and remaining integration work](docs/SDK_SETUP.md). See [implementation status and contracts](docs/IMPLEMENTATION_STATUS.md) for exact coverage and limitations.

From a Visual Studio 2022 Developer PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
npm test
```

The default build produces the static graphics core and contract tests. The original registry-writing executable is an explicit opt-in build target and is not run by tests.
