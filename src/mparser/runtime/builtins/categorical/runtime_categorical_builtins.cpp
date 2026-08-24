#include "mparser/runtime/builtins/categorical/runtime_categorical_builtins.h"

#include "mparser/runtime/core/value/runtime_categorical.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mparser {
namespace {

bool matches(std::string_view name,
             std::initializer_list<std::string_view> candidates) {
    return std::find(candidates.begin(), candidates.end(), name) !=
           candidates.end();
}

BuiltinResult failure(const BuiltinCall& call, std::string message) {
    return BuiltinResult::failure(
        call.span, std::move(message),
        "MParser:InvalidCategoricalCall");
}

BuiltinResult oneOutput(const BuiltinCall& call,
                        RuntimeCategoricalOperationResult result) {
    if (!result.succeeded) {
        return failure(call, std::move(result.error));
    }
    return call.requestedOutputCount == 0
               ? BuiltinResult::success()
               : BuiltinResult::success({std::move(result.value)});
}

BuiltinResult scalarOutput(const BuiltinCall& call, RuntimeValue value) {
    return call.requestedOutputCount == 0
               ? BuiltinResult::success()
               : BuiltinResult::success({std::move(value)});
}

std::optional<bool> logicalScalar(const RuntimeValue& value) {
    const auto truth = runtimeNumericTruthValue(value);
    return runtimeShapeElementCount(value) == 1 ? truth : std::nullopt;
}

bool isConstructorOption(std::string_view name) {
    return name == "Ordinal" || name == "Protected";
}

struct CategoricalConstructorArguments {
    bool succeeded = false;
    std::vector<RuntimeValue> positional;
    bool ordinal = false;
    bool protectedCategories = false;
    bool hasOrdinal = false;
    bool hasProtected = false;
    std::string error;
};

bool storeConstructorOption(
    CategoricalConstructorArguments& result, std::string_view name,
    const RuntimeValue& value) {
    const auto parsed = logicalScalar(value);
    if (!parsed) {
        result.error = std::string(name) +
                       " must be a logical scalar";
        return false;
    }
    if (name == "Ordinal") {
        if (result.hasOrdinal) {
            result.error = "Ordinal was supplied more than once";
            return false;
        }
        result.hasOrdinal = true;
        result.ordinal = *parsed;
        return true;
    }
    if (name == "Protected") {
        if (result.hasProtected) {
            result.error = "Protected was supplied more than once";
            return false;
        }
        result.hasProtected = true;
        result.protectedCategories = *parsed;
        return true;
    }
    result.error = "unsupported categorical option: " +
                   std::string(name);
    return false;
}

CategoricalConstructorArguments parseConstructor(
    const std::vector<RuntimeValue>& arguments) {
    CategoricalConstructorArguments result;
    if (arguments.empty()) {
        result.error = "categorical requires an input array";
        return result;
    }
    result.positional.push_back(arguments.front());
    size_t index = 1;
    while (index < arguments.size()) {
        const RuntimeValue& argument = arguments[index];
        if (argument.kind == RuntimeValueKind::NameValueArgument) {
            if (argument.cells.size() != 1 ||
                !isConstructorOption(argument.text) ||
                !storeConstructorOption(
                    result, argument.text, argument.cells.front())) {
                if (result.error.empty()) {
                    result.error =
                        "unsupported categorical name-value argument";
                }
                return result;
            }
            ++index;
            continue;
        }
        const auto name = runtimeTextScalarUtf8(argument);
        if (name && isConstructorOption(*name)) {
            if (index + 1 >= arguments.size()) {
                result.error = "categorical option is missing a value: " +
                               *name;
                return result;
            }
            if (!storeConstructorOption(
                    result, *name, arguments[index + 1])) {
                return result;
            }
            index += 2;
            continue;
        }
        result.positional.push_back(argument);
        ++index;
    }
    if (result.positional.size() > 3) {
        result.error =
            "categorical accepts data, value set, and category names before options";
        return result;
    }
    result.succeeded = true;
    return result;
}

bool emptyArgument(const RuntimeValue& value) {
    return runtimeShapeElementCount(value) == 0;
}

RuntimeValue categoryNameCell(const std::vector<std::string>& names) {
    std::vector<RuntimeValue> values;
    values.reserve(names.size());
    for (const std::string& name : names) {
        values.push_back(makeRuntimeCharacterVectorUtf8(name));
    }
    return makeRuntimeCellValue({names.size(), 1}, std::move(values));
}

std::optional<std::vector<std::string>> namesArgument(
    const RuntimeValue& value, std::string& error) {
    auto parsed = runtimeCategoricalNames(value, "category");
    if (!parsed.succeeded) {
        error = std::move(parsed.error);
        return std::nullopt;
    }
    return std::move(parsed.names);
}

} // namespace

bool isRuntimeCategoricalBuiltin(std::string_view name) {
    return matches(name, {
        "categorical", "iscategorical", "categories", "isundefined",
        "isordinal", "isprotected", "addcats", "removecats",
        "renamecats", "reordercats", "mergecats", "countcats"});
}

BuiltinResult invokeRuntimeCategoricalBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "categorical") {
        auto parsed = parseConstructor(call.arguments);
        if (!parsed.succeeded) {
            return failure(call, std::move(parsed.error));
        }
        const RuntimeValue* valueSet =
            parsed.positional.size() > 1 &&
                    !emptyArgument(parsed.positional[1])
                ? &parsed.positional[1]
                : nullptr;
        const RuntimeValue* categoryNames =
            parsed.positional.size() > 2 &&
                    !emptyArgument(parsed.positional[2])
                ? &parsed.positional[2]
                : nullptr;
        return oneOutput(
            call, runtimeConstructCategorical(
                      parsed.positional.front(), valueSet,
                      categoryNames, parsed.ordinal,
                      parsed.protectedCategories));
    }
    if (name == "iscategorical") {
        if (call.arguments.size() != 1) {
            return failure(call, "iscategorical expects one input");
        }
        return scalarOutput(
            call, makeRuntimeLogicalValue(
                      isRuntimeCategoricalValue(call.arguments.front())));
    }
    if (call.arguments.empty() ||
        !isRuntimeCategoricalValue(call.arguments.front())) {
        return failure(call, std::string(name) +
                                 " expects a categorical first input");
    }
    const auto* storage = runtimeCategoricalStorage(
        call.arguments.front());
    if (name == "categories") {
        if (call.arguments.size() != 1) {
            return failure(call, "categories expects one input");
        }
        return scalarOutput(call, categoryNameCell(storage->categories));
    }
    if (name == "isundefined") {
        if (call.arguments.size() != 1) {
            return failure(call, "isundefined expects one input");
        }
        return oneOutput(
            call, runtimeCategoricalMissingMask(call.arguments.front()));
    }
    if (name == "isordinal" || name == "isprotected") {
        if (call.arguments.size() != 1) {
            return failure(call, std::string(name) + " expects one input");
        }
        return scalarOutput(
            call, makeRuntimeLogicalValue(
                      name == "isordinal"
                          ? storage->ordinal
                          : storage->protectedCategories));
    }
    if (name == "countcats") {
        if (call.arguments.size() != 1) {
            return failure(call, "countcats expects one input");
        }
        return oneOutput(
            call, runtimeCategoricalCounts(call.arguments.front()));
    }

    std::string error;
    if (name == "addcats") {
        if (call.arguments.size() != 2 && call.arguments.size() != 4) {
            return failure(
                call,
                "addcats expects categories and optional Before/After anchor");
        }
        auto names = namesArgument(call.arguments[1], error);
        if (!names) {
            return failure(call, std::move(error));
        }
        std::string placement;
        std::string anchor;
        if (call.arguments.size() == 4) {
            const auto parsedPlacement =
                runtimeTextScalarUtf8(call.arguments[2]);
            const auto parsedAnchor =
                runtimeTextScalarUtf8(call.arguments[3]);
            if (!parsedPlacement || !parsedAnchor) {
                return failure(
                    call, "addcats placement and anchor must be text");
            }
            placement = *parsedPlacement;
            anchor = *parsedAnchor;
        }
        return oneOutput(
            call, runtimeAddCategories(
                      call.arguments.front(), std::move(*names),
                      placement, anchor));
    }
    if (name == "removecats" || name == "reordercats") {
        if (call.arguments.size() != 2) {
            return failure(call, std::string(name) +
                                     " expects a category list");
        }
        auto names = namesArgument(call.arguments[1], error);
        if (!names) {
            return failure(call, std::move(error));
        }
        return oneOutput(
            call, name == "removecats"
                      ? runtimeRemoveCategories(
                            call.arguments.front(), *names)
                      : runtimeReorderCategories(
                            call.arguments.front(), *names));
    }
    if (name == "renamecats") {
        if (call.arguments.size() != 3) {
            return failure(
                call, "renamecats expects old and new category lists");
        }
        auto oldNames = namesArgument(call.arguments[1], error);
        if (!oldNames) {
            return failure(call, std::move(error));
        }
        auto newNames = namesArgument(call.arguments[2], error);
        if (!newNames) {
            return failure(call, std::move(error));
        }
        return oneOutput(
            call, runtimeRenameCategories(
                      call.arguments.front(), *oldNames, *newNames));
    }
    if (name == "mergecats") {
        if (call.arguments.size() != 3) {
            return failure(
                call, "mergecats expects categories and a merged name");
        }
        auto names = namesArgument(call.arguments[1], error);
        const auto mergedName = runtimeTextScalarUtf8(call.arguments[2]);
        if (!names || !mergedName || mergedName->empty()) {
            return failure(
                call, names ? "mergecats name must be nonempty text"
                            : std::move(error));
        }
        return oneOutput(
            call, runtimeMergeCategories(
                      call.arguments.front(), *names, *mergedName));
    }
    return failure(call, "unsupported categorical builtin: " +
                             std::string(name));
}

} // namespace mparser
