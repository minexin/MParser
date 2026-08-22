#pragma once

#include <cstddef>
#include <cstdint>

namespace mparser::c_api_test {

enum class FaultPoint : std::uint32_t {
    None,
    SourceCopy,
    ModulePublish,
    SessionCreate,
    ExecuteBeforeCore,
    ExecuteAfterCore,
    ResultPublish,
    DiagnosticCopy,
    ValuePublish,
    ExternalValueCaches,
    CellCompose,
    StructCompose,
    CancelTokenCreate,
};

enum class ExceptionKind : std::uint32_t {
    BadAllocation,
    Unknown,
};

#if defined(MPARSER_C_API_TEST_FAULTS)
void arm(FaultPoint point, ExceptionKind kind,
         std::size_t matchingCallsBeforeFailure = 0) noexcept;
void clear() noexcept;
#endif

void inject(FaultPoint point);

} // namespace mparser::c_api_test
