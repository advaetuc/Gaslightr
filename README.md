# Gaslightr

Experimental Windows x64 Photoshop enhancement project using a headless D3D12 temporal pipeline.

The UXP frontend, bounded Imaging API orchestration, C++ tile processor, diagnostic Node-API binding, and real Adobe UXP addon are implemented. The `.uxpaddon` builds against the locally installed Adobe SDK and is staged with matching debugging symbols. **Photoshop loading and end-to-end inference remain unverified.** The inferred neural-runtime ABI still requires validation. Detail-Only mode is disabled pending CUDA interop repair.

Start with the [complete offline UXP setup and implementation guide](docs/OFFLINE_UXP_IMPLEMENTATION_GUIDE.md), including full source, SDK staging, build commands, directory-junction checks, file-log monitoring and Visual Studio native debugging. See [implementation status and contracts](docs/IMPLEMENTATION_STATUS.md) for exact coverage and limitations.

From a Visual Studio 2022 Developer PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
npm test
```

The default build produces the static graphics core and contract tests. The original registry-writing executable is an explicit opt-in build target and is not run by tests.
