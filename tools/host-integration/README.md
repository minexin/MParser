# MExecServer Graphics V3 Integration

This patch is an opt-in integration candidate, not a deployed server update.
Apply it to a separate MExecServer source checkout after checking the diff:

```powershell
git apply --check D:/project/MParser/tools/host-integration/mexecserver-graphics-v3.patch
git apply D:/project/MParser/tools/host-integration/mexecserver-graphics-v3.patch
```

Configure against the development MParser SDK using MParser_DIR and set
MEXEC_GRAPHICS_V3=ON. The default remains OFF. ON requires ABI generation 2,
revision 7 or later and the installed v3 schema. Both execution-result and
workspace-value serialization select v3 together.

Do not enable this server for the current v1-only browser client. This build
switch is not per-request protocol negotiation and does not implement
identity-targeted property writeback.

## Verification

Run the repository probe with the explicitly selected executable and DLL:

```powershell
./tools/probe-host-compile.ps1 -ServerExe <server-executable> -KernelDll <kernel-dll> -GraphicsV3
```

Omit GraphicsV3 for the legacy server. The probe starts an isolated loopback
session, verifies the loaded DLL path/hash, retains raw REST JSON under build,
and stops its own server. It checks matrix plotting, Text/Legend transport,
close/recreation, result/workspace identity agreement and export failures
without file creation or overwriting an existing file.

The isolated v3 binary was compiled from current MExecServer sources plus the
two patched serialization call sites, linked with existing third-party Oat++
libraries and the development MParser import library. This proves the selected
binary's REST behavior, not a clean downstream CMake build or deployment.

Latest paired local evidence:
- Legacy: build/host-compile-probe/9fbc4681980346279ee78908f117306d
- V3: build/host-compile-probe/e289ee0fbfab40c4b63e37af37ae95f9

Those build directories are local evidence, not committed release artifacts.
