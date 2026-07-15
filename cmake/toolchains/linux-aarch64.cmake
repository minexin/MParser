# Cross-compile for a GNU/Linux AArch64 target with the standard GNU triplet.
# Debian and Ubuntu provide these compilers in gcc/g++-aarch64-linux-gnu.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc CACHE FILEPATH
    "GNU C compiler targeting Linux AArch64")
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++ CACHE FILEPATH
    "GNU C++ compiler targeting Linux AArch64")
