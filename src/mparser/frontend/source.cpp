#include "mparser/frontend/source.h"

namespace mparser {

SourceSpan mergeSpans(SourceSpan first, SourceSpan second) {
    return SourceSpan{first.begin, second.end};
}

} // namespace mparser
