#pragma once

#include "mparser/source.h"

#include <filesystem>
#include <vector>

namespace mparser {

struct SourceLoaderOptions {
    std::vector<std::filesystem::path> classPaths;
};

struct SourceLoaderResult {
    std::vector<SourceUnit> sources;
};

class SourceLoader {
public:
    SourceLoaderResult load(
        const std::filesystem::path& entryPath,
        const SourceLoaderOptions& options = {}) const;
};

} // namespace mparser
