#pragma once

#include <cstdint>
#include <memory>

namespace mparser {

// A token has no execution capability. Copies identify an already-recorded
// event across internal forwarding without retaining a journal or runtime.
struct ExecutionEventIdentity {};
struct ExecutionEventStamp {
    std::shared_ptr<const ExecutionEventIdentity> identity;
    std::uint64_t sequence = 0;
};

} // namespace mparser
