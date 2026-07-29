#include "consumer_module.h"

#include "mparser/cpp_api.hpp"

std::uint32_t mparserInstalledCppApiVersionToken() noexcept {
    const auto version = mparser::sdk::sourceApiVersion();
    return version.major * 100u + version.minor;
}
