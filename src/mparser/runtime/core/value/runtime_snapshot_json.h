#pragma once
#include "mparser/runtime/core/value/runtime_value_snapshot.h"

namespace mparser {
// A pointer-free, non-executable graph containing only public reachable edges.
std::string runtimePublicSnapshotJson(const RuntimeValueSnapshot& snapshot,RuntimeSnapshotLimits limits = {});
}
