#pragma once

#include "mparser/runtime/core/object_model/runtime_object.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

enum class RuntimeLvalueSegmentKind {
    Member,
    Parenthesis,
    Brace,
};

struct RuntimeLvalueSegment {
    RuntimeLvalueSegmentKind kind = RuntimeLvalueSegmentKind::Member;
    std::string memberName;
    std::vector<RuntimeValue> subscripts;
    std::vector<bool> colonSubscripts;
};

struct RuntimeLvalueOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

using RuntimeObjectMemberReader = std::function<
    RuntimeLvalueOperationResult(const RuntimeValue&, std::string_view)>;
using RuntimeObjectMemberWriter = std::function<
    RuntimeLvalueOperationResult(const RuntimeValue&, std::string_view,
                                 const RuntimeValue&)>;

struct RuntimeLvalueHooks {
    RuntimeObjectMemberReader readObjectMember;
    RuntimeObjectMemberWriter writeObjectMember;
    RuntimeObjectArrayPolicy objectArrays;
};

class RuntimeLvalueTransaction {
public:
    explicit RuntimeLvalueTransaction(RuntimeValue root);

    const RuntimeValue& current() const;
    const RuntimeValue& root() const;

    RuntimeLvalueOperationResult descend(
        RuntimeLvalueSegment segment,
        const RuntimeLvalueHooks& hooks = {},
        std::optional<RuntimeValue> missingMemberSeed = std::nullopt);

    RuntimeLvalueOperationResult assign(
        RuntimeLvalueSegment segment, const RuntimeValue& value,
        bool nullAssignment, const RuntimeLvalueHooks& hooks = {});

private:
    struct Frame {
        RuntimeValue parent;
        RuntimeLvalueSegment segment;
    };

    RuntimeValue root_;
    RuntimeValue current_;
    std::vector<Frame> frames_;
};

} // namespace mparser
