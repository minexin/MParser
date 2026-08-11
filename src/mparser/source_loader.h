#pragma once

#include "mparser/source.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mparser {

struct SourceLoaderOptions {
    std::vector<std::filesystem::path> searchPaths;
};

struct SourceLoaderResult {
    std::vector<SourceUnit> sources;
};

class SourceLoader {
public:
    SourceLoaderResult load(
        const std::filesystem::path& entryPath,
        const SourceLoaderOptions& options = {}) const;
    SourceLoaderResult loadSource(
        const std::filesystem::path& sourceName,
        std::string source,
        const SourceLoaderOptions& options = {}) const;
};

} // namespace mparser
