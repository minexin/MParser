#pragma once

#include <cstddef>

namespace mparser {

struct SourcePosition {
    size_t offset = 0;
    int line = 1;
    int column = 1;
};

struct SourceSpan {
    SourcePosition begin;
    SourcePosition end;
};

SourceSpan mergeSpans(SourceSpan first, SourceSpan second);

} // namespace mparser
