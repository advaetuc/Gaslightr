# Gaslightr

Current mode: **D3D12 GPU copy validation**, with `neuralInference: false`. The user confirmed addon loading, six boundary checks, and an initial live canvas/seam/undo test. The current frontend regression fixes have 25 passing mock-host tests and await a Photoshop retest. Read the [master engineering handoff](GASLIGHTR_AGENT_CONTEXT.md) for current status and the pending Phase 8A/8B decision. The validation path never loads the staged neural DLL.

Experimental Windows x64 Photoshop enhancement project using a headless D3D12 temporal pipeline.

The UXP frontend, bounded Imaging API orchestration, C++ tile processor, diagnostic Node-API binding, and real Adobe UXP addon are implemented. The `.uxpaddon` builds against the locally installed Adobe SDK and is staged with matching debugging symbols. Neural integration requires a verified runtime contract; the inferred ABI must not be called. Detail-Only mode is disabled pending CUDA interop repair.

Start with the [complete offline UXP setup and implementation guide](docs/OFFLINE_UXP_IMPLEMENTATION_GUIDE.md), including full source, SDK staging, build commands, directory-junction checks, file-log monitoring and Visual Studio native debugging. See [implementation status and contracts](docs/IMPLEMENTATION_STATUS.md) for exact coverage and limitations.

From a Visual Studio 2022 Developer PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
npm test
```

The default build produces the static graphics core and contract tests. The original registry-writing executable is an explicit opt-in build target and is not run by tests.
