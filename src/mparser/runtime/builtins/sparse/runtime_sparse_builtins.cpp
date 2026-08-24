#include "mparser/runtime/builtins/sparse/runtime_sparse_builtins.h"

#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_sparse.h"
#include "mparser/runtime/core/value/runtime_shape.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>

namespace mparser {
namespace {

bool matches(std::string_view name,
             std::initializer_list<std::string_view> candidates) {
    return std::find(candidates.begin(), candidates.end(), name) !=
           candidates.end();
}

BuiltinResult failure(const BuiltinCall& call, std::string message) {
    return BuiltinResult::failure(
        call.span, std::move(message), "MParser:InvalidSparseCall");
}

BuiltinResult oneOutput(const BuiltinCall& call,
                        RuntimeSparseOperationResult result) {
    if (!result.succeeded) {
        return failure(call, std::move(result.error));
    }
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success();
    }
    return BuiltinResult::success({std::move(result.value)});
}

BuiltinResult scalarOutput(const BuiltinCall& call, RuntimeValue value) {
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success();
    }
    return BuiltinResult::success({std::move(value)});
}

} // namespace

bool isRuntimeSparseBuiltin(std::string_view name) {
    return matches(name, {"sparse", "spalloc", "speye", "spones",
                          "full", "nonzeros", "nnz", "issparse"});
}

BuiltinResult invokeRuntimeSparseBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "sparse") {
        return oneOutput(call, runtimeConstructSparse(call.arguments));
    }
    if (name == "spalloc") {
        return oneOutput(call, runtimeSpalloc(call.arguments));
    }
    if (name == "speye") {
        return oneOutput(call, runtimeSpeye(call.arguments));
    }
    if (name == "spones") {
        if (call.arguments.size() != 1) {
            return failure(call, "spones expects one input");
        }
        return oneOutput(call,
                         runtimeSparseSpones(call.arguments.front()));
    }
    if (name == "full") {
        if (call.arguments.size() != 1) {
            return failure(call, "full expects one input");
        }
        if (isRuntimeSparseValue(call.arguments.front())) {
            return oneOutput(call,
                             runtimeSparseToFull(call.arguments.front()));
        }
        if (!isRuntimeNumericValue(call.arguments.front())) {
            return failure(call, "full requires a numeric input");
        }
        return scalarOutput(call, call.arguments.front());
    }
    if (name == "nonzeros") {
        if (call.arguments.size() != 1) {
            return failure(call, "nonzeros expects one input");
        }
        return oneOutput(call,
                         runtimeSparseNonzeros(call.arguments.front()));
    }
    if (name == "nnz") {
        if (call.arguments.size() != 1 ||
            !isRuntimeNumericValue(call.arguments.front())) {
            return failure(call, "nnz requires one numeric input");
        }
        size_t count = 0;
        if (const RuntimeSparseStorage* storage =
                runtimeSparseStorage(call.arguments.front())) {
            count = storage->values.size();
        } else {
            const size_t elements =
                runtimeShapeElementCount(call.arguments.front());
            for (size_t index = 0; index < elements; ++index) {
                const auto value = runtimeNumericElementValue(
                    call.arguments.front(), index);
                if (value &&
                    (value->real != 0.0 ||
                     (value->complex && value->imaginary != 0.0))) {
                    ++count;
                }
            }
        }
        return scalarOutput(call,
                            makeRuntimeNumberValue(
                                static_cast<double>(count)));
    }
    if (name == "issparse") {
        if (call.arguments.size() != 1) {
            return failure(call, "issparse expects one input");
        }
        return scalarOutput(call,
                            makeRuntimeLogicalValue(
                                isRuntimeSparseValue(call.arguments.front())));
    }
    return failure(call, "unsupported sparse builtin: " + std::string(name));
}

} // namespace mparser
