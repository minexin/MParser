#include "mparser/runtime/core/indexing/runtime_lvalue.h"

#include "mparser/runtime/core/indexing/runtime_assignment.h"
#include "mparser/runtime/core/value/runtime_cell.h"
#include "mparser/runtime/core/session/runtime_exception.h"
#include "mparser/runtime/core/indexing/runtime_index.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"

#include <utility>

namespace mparser {
namespace {

RuntimeLvalueOperationResult failure(std::string error = {}) {
    return RuntimeLvalueOperationResult{false, {}, std::move(error)};
}

RuntimeLvalueOperationResult success(RuntimeValue value) {
    return RuntimeLvalueOperationResult{true, std::move(value), {}};
}

bool isObject(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Object &&
           !isRuntimeException(value);
}

bool isLinearColon(const RuntimeLvalueSegment& segment) {
    return segment.subscripts.size() == 1 &&
           segment.colonSubscripts.size() == 1 &&
           segment.colonSubscripts.front();
}

RuntimeLvalueOperationResult readMember(
    const RuntimeValue& parent, std::string_view name,
    const RuntimeLvalueHooks& hooks,
    const std::optional<RuntimeValue>& missingSeed) {
    if (parent.kind == RuntimeValueKind::Struct) {
        auto field = runtimeStructFieldValues(parent, name);
        if (!field.succeeded) {
            if (missingSeed && isRuntimeScalarStruct(parent) &&
                field.error.find("field is not available") !=
                    std::string::npos) {
                return success(*missingSeed);
            }
            return failure(std::move(field.error));
        }
        return success(std::move(field.value));
    }
    if (isRuntimeException(parent)) {
        const RuntimeValue* property =
            runtimeExceptionProperty(parent, name);
        return property
                   ? success(*property)
                   : failure("MException property is not available: " +
                             std::string(name));
    }
    if (isObject(parent)) {
        if (!hooks.readObjectMember) {
            return failure(
                "object member access is not available in this execution tier");
        }
        return hooks.readObjectMember(parent, name);
    }
    return failure("member access requires a structure or object target");
}

RuntimeLvalueOperationResult readSegment(
    const RuntimeValue& parent, const RuntimeLvalueSegment& segment,
    const RuntimeLvalueHooks& hooks,
    const std::optional<RuntimeValue>& missingSeed) {
    switch (segment.kind) {
    case RuntimeLvalueSegmentKind::Member:
        return readMember(parent, segment.memberName, hooks, missingSeed);
    case RuntimeLvalueSegmentKind::Parenthesis: {
        const bool linearColon = isLinearColon(segment);
        if (parent.kind == RuntimeValueKind::Struct) {
            auto result = runtimeIndexStruct(parent, segment.subscripts,
                                             linearColon);
            return result.succeeded
                       ? success(std::move(result.value))
                       : failure(std::move(result.error));
        }
        if (parent.kind == RuntimeValueKind::Cell) {
            auto result = runtimeIndexCell(parent, segment.subscripts,
                                           linearColon);
            return result.succeeded
                       ? success(std::move(result.value))
                       : failure(std::move(result.error));
        }
        if (isRuntimeTextValue(parent)) {
            auto result = runtimeIndexText(parent, segment.subscripts,
                                           linearColon);
            return result.succeeded
                       ? success(std::move(result.value))
                       : failure(std::move(result.error));
        }
        if (parent.kind == RuntimeValueKind::MissingArray) {
            auto result = runtimeIndexMissingArray(
                parent, segment.subscripts, linearColon);
            return result.succeeded
                       ? success(std::move(result.value))
                       : failure(std::move(result.error));
        }
        if (isRuntimeClassObject(parent)) {
            auto result = runtimeIndexObject(
                parent, segment.subscripts, hooks.objectArrays,
                linearColon);
            return result.succeeded
                       ? success(std::move(result.value))
                       : failure(std::move(result.error));
        }
        {
            auto result = runtimeIndexNumeric(parent, segment.subscripts,
                                              linearColon);
            return result.succeeded
                       ? success(std::move(result.value))
                       : failure(std::move(result.error));
        }
    }
    case RuntimeLvalueSegmentKind::Brace: {
        if (isRuntimeStringArray(parent)) {
            auto result = runtimeIndexStringContents(
                parent, segment.subscripts);
            return result.succeeded
                       ? success(std::move(result.value))
                       : failure(std::move(result.error));
        }
        auto result = runtimeIndexCellContents(parent, segment.subscripts);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    }
    return failure("unsupported lvalue path segment");
}

RuntimeLvalueOperationResult writeMember(
    RuntimeValue parent, std::string name, const RuntimeValue& value,
    const RuntimeLvalueHooks& hooks) {
    if (parent.kind == RuntimeValueKind::Missing) {
        parent = makeRuntimeStructValue();
    }
    if (parent.kind == RuntimeValueKind::Struct) {
        if (!runtimeSetStructField(parent, std::move(name), value)) {
            return failure(
                "direct field assignment requires a scalar structure");
        }
        return success(std::move(parent));
    }
    if (isRuntimeException(parent)) {
        return failure("MException properties are read-only: " + name);
    }
    if (isObject(parent)) {
        if (!hooks.writeObjectMember) {
            return failure(
                "object member assignment is not available in this execution tier");
        }
        return hooks.writeObjectMember(parent, name, value);
    }
    return failure("member assignment requires a structure or object target");
}

RuntimeLvalueOperationResult writeSegment(
    RuntimeValue parent, const RuntimeLvalueSegment& segment,
    const RuntimeValue& value, bool nullAssignment,
    const RuntimeLvalueHooks& hooks) {
    switch (segment.kind) {
    case RuntimeLvalueSegmentKind::Member:
        return writeMember(std::move(parent), segment.memberName, value,
                           hooks);
    case RuntimeLvalueSegmentKind::Parenthesis:
        if (parent.kind == RuntimeValueKind::Struct) {
            auto result = nullAssignment
                              ? runtimeDeleteStructIndexed(
                                    parent, segment.subscripts)
                              : runtimeAssignStructIndexed(
                                    parent, segment.subscripts, value);
            return result.succeeded
                       ? success(std::move(result.value))
                       : failure(std::move(result.error));
        }
        if (parent.kind == RuntimeValueKind::Cell) {
            auto result = nullAssignment
                              ? runtimeDeleteCellIndexed(
                                    parent, segment.subscripts,
                                    segment.colonSubscripts)
                              : runtimeAssignCellIndexed(
                                    parent, segment.subscripts, value);
            return result.succeeded
                       ? success(std::move(result.value))
                       : failure(std::move(result.error));
        }
        if (isRuntimeTextValue(parent)) {
            auto result = nullAssignment
                              ? runtimeDeleteTextIndexed(
                                    parent, segment.subscripts,
                                    segment.colonSubscripts)
                              : runtimeAssignTextIndexed(
                                    parent, segment.subscripts, value);
            return result.succeeded
                       ? success(std::move(parent))
                       : failure(std::move(result.error));
        }
        if (parent.kind == RuntimeValueKind::MissingArray) {
            auto result = nullAssignment
                              ? runtimeDeleteMissingIndexed(
                                    parent, segment.subscripts,
                                    segment.colonSubscripts)
                              : runtimeAssignMissingIndexed(
                                    parent, segment.subscripts, value);
            return result.succeeded
                       ? success(std::move(parent))
                       : failure(std::move(result.error));
        }
        if (isRuntimeClassObject(parent)) {
            auto result = nullAssignment
                              ? runtimeDeleteObjectIndexed(
                                    parent, segment.subscripts,
                                    segment.colonSubscripts,
                                    hooks.objectArrays)
                              : runtimeAssignObjectIndexed(
                                    parent, segment.subscripts, value,
                                    hooks.objectArrays);
            return result.succeeded
                       ? success(std::move(result.value))
                       : failure(std::move(result.error));
        }
        if (nullAssignment) {
            auto result = runtimeDeleteNumericIndexed(
                parent, segment.subscripts, segment.colonSubscripts);
            return result.succeeded
                       ? success(std::move(parent))
                       : failure(std::move(result.error));
        }
        {
            auto result = runtimeAssignNumericIndexed(
                parent, segment.subscripts, value);
            return result.succeeded
                       ? success(std::move(parent))
                       : failure(std::move(result.error));
        }
    case RuntimeLvalueSegmentKind::Brace: {
        if (isRuntimeStringArray(parent)) {
            auto result = runtimeAssignStringContents(
                parent, segment.subscripts, value);
            return result.succeeded
                       ? success(std::move(parent))
                       : failure(std::move(result.error));
        }
        auto result = runtimeAssignCellContents(
            parent, segment.subscripts, value);
        return result.succeeded
                   ? success(std::move(result.value))
                   : failure(std::move(result.error));
    }
    }
    return failure("unsupported lvalue path segment");
}

} // namespace

RuntimeLvalueTransaction::RuntimeLvalueTransaction(RuntimeValue root)
    : root_(std::move(root)), current_(root_) {}

const RuntimeValue& RuntimeLvalueTransaction::current() const {
    return current_;
}

const RuntimeValue& RuntimeLvalueTransaction::root() const {
    return root_;
}

RuntimeLvalueOperationResult RuntimeLvalueTransaction::descend(
    RuntimeLvalueSegment segment, const RuntimeLvalueHooks& hooks,
    std::optional<RuntimeValue> missingMemberSeed) {
    RuntimeValue parent = current_;
    if (parent.kind == RuntimeValueKind::Missing) {
        if (segment.kind == RuntimeLvalueSegmentKind::Member) {
            parent = makeRuntimeStructValue();
        } else if (missingMemberSeed) {
            parent = *missingMemberSeed;
        }
    }
    if (!missingMemberSeed &&
        segment.kind == RuntimeLvalueSegmentKind::Member) {
        missingMemberSeed = makeRuntimeStructValue();
    }

    if (parent.kind == RuntimeValueKind::Struct &&
        segment.kind == RuntimeLvalueSegmentKind::Parenthesis) {
        auto capacity = runtimeEnsureStructIndexedCapacity(
            parent, segment.subscripts);
        if (!capacity.succeeded) {
            return failure(std::move(capacity.error));
        }
        parent = std::move(capacity.value);
    }
    if (isRuntimeClassObject(parent) &&
        segment.kind == RuntimeLvalueSegmentKind::Parenthesis) {
        auto capacity = runtimeEnsureObjectIndexedCapacity(
            parent, segment.subscripts, hooks.objectArrays);
        if (!capacity.succeeded) {
            return failure(std::move(capacity.error));
        }
        parent = std::move(capacity.value);
    }

    auto child = readSegment(parent, segment, hooks, missingMemberSeed);
    if (!child.succeeded && missingMemberSeed &&
        parent.kind == RuntimeValueKind::Cell &&
        segment.kind == RuntimeLvalueSegmentKind::Brace &&
        child.error.find("out of bounds") != std::string::npos) {
        auto grown = runtimeAssignCellContents(
            parent, segment.subscripts, *missingMemberSeed);
        if (!grown.succeeded) {
            return failure(std::move(grown.error));
        }
        parent = std::move(grown.value);
        child = readSegment(parent, segment, hooks, missingMemberSeed);
    }
    if (!child.succeeded) {
        return child;
    }
    const auto single = runtimeRequireSingleValue(
        child.value, "nested assignment target");
    if (!single.succeeded) {
        return failure(std::move(single.error));
    }

    frames_.push_back(Frame{std::move(parent), std::move(segment)});
    current_ = single.value;
    return success(current_);
}

RuntimeLvalueOperationResult RuntimeLvalueTransaction::assign(
    RuntimeLvalueSegment segment, const RuntimeValue& value,
    bool nullAssignment, const RuntimeLvalueHooks& hooks) {
    const auto single = runtimeRequireSingleValue(
        value, "assignment right-hand side");
    if (!single.succeeded) {
        return failure(std::move(single.error));
    }

    auto updated = writeSegment(current_, segment, single.value,
                                nullAssignment, hooks);
    if (!updated.succeeded) {
        return updated;
    }
    for (auto frame = frames_.rbegin(); frame != frames_.rend(); ++frame) {
        RuntimeValue parent = std::move(frame->parent);
        if (parent.kind == RuntimeValueKind::Struct &&
            frame->segment.kind ==
                RuntimeLvalueSegmentKind::Parenthesis &&
            updated.value.kind == RuntimeValueKind::Struct) {
            auto aligned = runtimeAlignStructSchemaForCopyback(
                parent, updated.value);
            if (!aligned.succeeded) {
                return failure(std::move(aligned.error));
            }
            parent = std::move(aligned.value);
        }
        updated = writeSegment(std::move(parent), frame->segment,
                               updated.value, false, hooks);
        if (!updated.succeeded) {
            return updated;
        }
    }
    root_ = updated.value;
    current_ = updated.value;
    frames_.clear();
    return success(root_);
}

} // namespace mparser
