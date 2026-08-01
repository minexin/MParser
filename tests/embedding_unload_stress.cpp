#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr std::size_t kLoadCount = 256;

using VersionFunction = std::uint32_t (*)();

#if defined(_WIN32)

template <typename Function>
Function loadSymbol(HMODULE library, const char* name) {
    const FARPROC symbol = GetProcAddress(library, name);
    static_assert(sizeof(Function) == sizeof(symbol));
    return std::bit_cast<Function>(symbol);
}

void runOneLoad(const std::filesystem::path& libraryPath) {
    const HMODULE library =
        LoadLibraryW(libraryPath.c_str());
    assert(library);

    const auto versionMajor =
        loadSymbol<VersionFunction>(
            library, "mparser_version_major");
    const auto abiMajor =
        loadSymbol<VersionFunction>(
            library, "mparser_c_abi_version");
    const auto abiRevision =
        loadSymbol<VersionFunction>(
            library, "mparser_c_abi_revision");
    assert(versionMajor && abiMajor && abiRevision);
    assert(versionMajor() == 0);
    assert(abiMajor() == 1);
    assert(abiRevision() >= 1);

    assert(FreeLibrary(library) != 0);
}

#else

template <typename Function>
Function loadSymbol(void* library, const char* name) {
    return reinterpret_cast<Function>(
        dlsym(library, name));
}

void runOneLoad(const std::filesystem::path& libraryPath) {
    void* library = dlopen(
        libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    assert(library);

    const auto versionMajor =
        loadSymbol<VersionFunction>(
            library, "mparser_version_major");
    const auto abiMajor =
        loadSymbol<VersionFunction>(
            library, "mparser_c_abi_version");
    const auto abiRevision =
        loadSymbol<VersionFunction>(
            library, "mparser_c_abi_revision");
    assert(versionMajor && abiMajor && abiRevision);
    assert(versionMajor() == 0);
    assert(abiMajor() == 1);
    assert(abiRevision() >= 1);

    assert(dlclose(library) == 0);
}

#endif

} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path libraryPath(argv[1]);
    assert(std::filesystem::is_regular_file(libraryPath));
    for (std::size_t iteration = 0;
         iteration < kLoadCount; ++iteration) {
        runOneLoad(libraryPath);
    }

    std::cout << "embedding unload stress = "
              << kLoadCount << ",abi-1.1\n";
    return 0;
}
