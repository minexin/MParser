# Build And Install

MParser builds from the checked-in source tree without downloading its native
JIT dependency. The pinned SLJIT source is vendored under
`third_party/sljit`.

## Requirements

- CMake 3.20 or newer;
- a C++20 compiler and a C11 compiler;
- a build tool supported by CMake; Ninja is used by the checked-in presets;
- Windows x64, Linux x64/AArch64, or macOS x64/ARM64 for release-target
  configurations.

The current release-target toolchains are MSVC on Windows, GCC or Clang on
Linux, and Apple Clang on macOS. Linux AArch64 may be cross-compiled with the
checked-in toolchain, but physical ARM hardware is required for publishable
performance measurements.

## Windows With MSVC

Run from an x64 Visual Studio Developer Command Prompt, or activate
`vcvars64.bat` before configuring. The wrapper keeps CMake's localized MSVC
dependency output in UTF-8:

```powershell
.\cmake\configure-windows-msvc.cmd
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

The preset uses Ninja, Release, the bundled native JIT, and
`build/windows-msvc-release`.

If an old build tree was configured with another compiler, generator, or code
page, configure a new directory. CMake 3.24 or newer may instead use
`cmake --fresh --preset windows-msvc-release`.

### Windows MSVC AddressSanitizer

The checked-in local validation preset instruments the complete no-JIT build
with MSVC AddressSanitizer:

```powershell
cmake --preset windows-msvc-asan-nojit
cmake --build --preset windows-msvc-asan-nojit --parallel
ctest --preset windows-msvc-asan-nojit
```

Run all three commands from the same x64 Visual Studio developer environment.
The final installed-consumer tests invoke CMake and the compiler again, so a
plain PowerShell process without the Windows SDK `LIB` and `INCLUDE`
environment is insufficient.

The preset selects `RelWithDebInfo`, `MPARSER_ENABLE_NATIVE_JIT=OFF`, and
`MPARSER_ENABLE_MSVC_ASAN=ON`. CMake adds `/fsanitize=address`, debug
information, and `/INCREMENTAL:NO`, locates the architecture-matched
`clang_rt.asan_dynamic-*.dll` beside the selected `cl.exe`, and stages it only
into local test output directories. The DLL is neither vendored nor installed
or packaged by MParser. The option rejects native-JIT and release-packaging
configurations so this diagnostic build cannot be mistaken for a release
artifact.

## Linux Or macOS

The Linux preset uses Ninja and Release:

```bash
cmake --preset linux-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release
```

The equivalent portable configuration, also suitable for macOS, is:

```bash
cmake -S . -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMPARSER_ENABLE_NATIVE_JIT=ON
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure
```

## Native And No-JIT Builds

Native JIT is enabled by default:

```text
-DMPARSER_ENABLE_NATIVE_JIT=ON
```

It uses `third_party/sljit` and requires no network request. An already
available compatible SLJIT checkout can be selected explicitly:

```text
-DMPARSER_SLJIT_SOURCE_DIR=/path/to/sljit
```

Build without machine-code generation with:

```bash
cmake -S . -B build/nojit -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMPARSER_ENABLE_NATIVE_JIT=OFF
cmake --build build/nojit --parallel
ctest --test-dir build/nojit --output-on-failure
```

No-JIT builds retain the parser, semantic analysis, bytecode VM, production
`--run`, and portable typed execution. They remove only the SLJIT native
backend.

## Linux AArch64 Cross Build

On a Debian/Ubuntu x64 host with the GNU AArch64 cross compiler:

```bash
cmake -S . -B build/linux-aarch64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64.cmake \
  -DMPARSER_ENABLE_NATIVE_JIT=ON
cmake --build build/linux-aarch64 --parallel
```

Execution tests require an AArch64 host or a correctly configured
`CMAKE_CROSSCOMPILING_EMULATOR`. Emulated execution is correctness evidence,
not native performance evidence.

## Testing

`BUILD_TESTING` is enabled by default through CTest. A normal validation is:

```bash
ctest --test-dir build/release --output-on-failure
```

For an install-only SDK build:

```bash
cmake -S . -B build/sdk -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build/sdk --parallel
```

The engineering performance collector is built only with testing enabled and
is not part of the installed SDK.

## Install

Install to an explicit prefix:

```bash
cmake --install build/release --prefix /absolute/path/to/mparser-sdk
```

For a multi-configuration generator, add `--config Release`. The installed
tree contains:

- `bin/mparser` and the C shared runtime;
- `include/mparser/c_api.h` and `include/mparser/cpp_api.hpp`;
- exported `MParserConfig.cmake` targets;
- schemas, contract manifests, manuals, license, and notices;
- C, C++, machine-protocol, reliability, performance, and class-folder
  examples.

On Windows, place the C runtime DLL beside the host executable or on the DLL
search path.

## Consume With CMake

Pure C:

```cmake
find_package(MParser CONFIG REQUIRED COMPONENTS C CLI)
add_executable(host main.c)
target_link_libraries(host PRIVATE MParser::c_api)
set_property(TARGET host PROPERTY C_STANDARD 11)
```

C++20:

```cmake
find_package(MParser CONFIG REQUIRED COMPONENTS CPP CLI)
add_executable(host main.cpp)
target_link_libraries(host PRIVATE MParser::cpp_api)
target_compile_features(host PRIVATE cxx_std_20)
```

Set `CMAKE_PREFIX_PATH` or `MParser_DIR` to the installed prefix when CMake
cannot locate the package. Exported targets are:

- `MParser::c_api`: shared C ABI runtime;
- `MParser::cpp_api`: header-only C++ facade linked to the C runtime;
- `MParser::cli`: imported `mparser` executable.

The package also exports contract versions and paths such as
`MParser_C_ABI_VERSION`, `MParser_C_ABI_REVISION`,
`MParser_CPP_API_VERSION_MAJOR`, `MParser_CLI_CONTRACT_MAJOR`,
`MParser_MACHINE_PROTOCOL_MAJOR`, and `MParser_PUBLIC_CONTRACT_FILE`.

## Build A Release Archive

Release packaging is opt-in:

```bash
cmake -S . -B build/package -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMPARSER_ENABLE_RELEASE_PACKAGING=ON
cmake --build build/package --parallel
cmake --build build/package --target mparser_release_package
```

Windows emits a ZIP; Linux and macOS emit a `.tar.gz`. The publication target
requires a clean Git worktree. The package directory receives:

- the platform/architecture archive;
- CPack's `<archive>.sha256` sidecar;
- `<archive>.provenance.json`, an unsigned in-toto Statement v1 using the
  SLSA Provenance v1 predicate;
- `SHA256SUMS`, binding the archive and provenance statement.

`release_archive_smoke` creates the payload and statement twice with
normalized metadata, compares both hashes, relocates the unpacked SDK, builds
independent C11 and C++20 consumers, and runs the unpacked CLI machine
protocol. The local unsigned statement records inputs and the builder boundary
but does not authenticate publisher identity or claim a SLSA level. Final
publication still requires the selected signing or hosted-provenance
mechanism. See [v1 Release Process](release-process.md).

## Troubleshooting

- If `cl` is missing, activate the Visual Studio developer environment before
  deciding that the compiler is not installed.
- If Ninja reports a dependency-prefix parse failure on localized Windows,
  reconfigure through `cmake/configure-windows-msvc.cmd`.
- If CMake keeps an old compiler or architecture, use a new build directory.
- If `--jit=native` is unavailable, confirm the build was configured with
  `MPARSER_ENABLE_NATIVE_JIT=ON` and that the target architecture is supported.
- If an installed executable cannot load the C runtime, fix the platform
  shared-library search path; do not link against internal static targets.
- Never infer target performance from QEMU or another instruction emulator.

See [C Embedding API](embedding-c-api.md) and
[C++ Embedding SDK](embedding-cpp-api.md) for complete host examples.
