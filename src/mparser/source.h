#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace mparser {

inline constexpr size_t kInvalidSourceId =
    std::numeric_limits<size_t>::max();

struct SourceFunctionBinding {
    std::string alias;
    std::string target;
};

struct SourceUnit {
    std::string name;
    std::string content;
    std::string namespaceName;
    std::string primaryFunctionIdentity;
    std::string classMethodOwner;
    std::vector<SourceFunctionBinding> functionBindings;
};

struct SourcePosition {
    size_t offset = 0;
    int line = 1;
    int column = 1;
    size_t sourceId = kInvalidSourceId;
};

struct SourceSpan {
    SourcePosition begin;
    SourcePosition end;
};

SourceSpan mergeSpans(SourceSpan first, SourceSpan second);

} // namespace mparser
