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
