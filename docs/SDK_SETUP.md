# Gaslightr SDK setup

Use the [complete offline UXP setup and implementation guide](OFFLINE_UXP_IMPLEMENTATION_GUIDE.md). It replaces the earlier instructions that depended on UXP Developer Tool and an adapter that had not yet been written.

The installed SDK roots are `C:\SDKs\Adobe\Photoshop` and `C:\SDKs\Adobe\UXP-Hybrid`. The real Adobe addon now builds as `uxp-frontend/win/x64/gaslightr.uxpaddon`; it uses manifest 6, as required by this installed SDK. Host loading and neural inference remain unverified. See [implementation status](IMPLEMENTATION_STATUS.md).

The guide contains the complete adapter and CMake definitions, local SDK copy script, offline build commands, junction verification, host-log monitoring, native debugging and acceptance tests. No UDT, Creative Cloud GUI, developer port or remote-debugging connection is required by those procedures.

## Optional Node diagnostic target

This target is unrelated to Photoshop's addon loader. The previously staged Node headers and import library must match the installed Node runtime. From Developer PowerShell for VS 2022:

```powershell
Set-Location 'C:\Users\Advaet\Documents\Projects\Gaslightr'
cmake -S . -B build -DGASLIGHTR_BUILD_NODE_BINDING=ON `
    "-DNODE_API_INCLUDE_DIR=$PWD/vendor/node/node-v24.19.0/include/node" `
    "-DNODE_IMPORT_LIBRARY=$PWD/vendor/node/node.lib"
if ($LASTEXITCODE -ne 0) { throw 'Configuration failed' }
cmake --build build --config Release --target GaslightrNode
if ($LASTEXITCODE -ne 0) { throw 'Node diagnostic build failed' }
& 'C:\Program Files\nodejs\node.exe' --test tests/native.test.js
if ($LASTEXITCODE -ne 0) { throw 'Node diagnostic tests failed' }
```

Never rename `gaslightr.node` to `.uxpaddon`. The Adobe addon uses a different ABI and requires its own compiled adapter.
