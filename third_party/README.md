# Third-Party Sources

## SLJIT

MParser vendors the complete upstream `sljit_src` directory required by
`sljitLir.c`, plus the upstream license, README, and API change log.

- Upstream: https://github.com/zherczeg/sljit
- Revision: `3907e69005ba6e30b225000f24aaef3632f88347`
- License: Simplified BSD
- Local path: `third_party/sljit`

The default CMake build uses this copy and performs no dependency download.
`MPARSER_SLJIT_SOURCE_DIR` may point to another source tree for backend update
testing. Keep the pinned revision and `THIRD_PARTY_NOTICES.md` synchronized
when updating the bundled source.

## miniz

MParser vendors the zlib-compatible compression subset used to read and write
compressed MAT v5 elements. ZIP APIs are disabled and not compiled.

- Upstream: https://github.com/richgel999/miniz
- Version: `3.1.2`
- Revision: `77d0dce8627735138c51770d1799a1ef48f2117d`
- License: MIT
- Local path: `third_party/miniz`

The default build uses this pinned local copy and performs no dependency
download.
