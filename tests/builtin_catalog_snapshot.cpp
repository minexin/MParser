#include "mparser/runtime/builtins/builtin_registry.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::ordered_json;

std::string_view implementationName(
    mparser::BuiltinImplementationKind value) {
    using Kind = mparser::BuiltinImplementationKind;
    switch (value) {
    case Kind::Shared:
        return "shared";
    case Kind::Context:
        return "context";
    case Kind::Intrinsic:
        return "intrinsic";
    case Kind::Unsupported:
        return "unsupported";
    }
    return "unknown";
}

std::string_view purityName(mparser::BuiltinPurity value) {
    using Purity = mparser::BuiltinPurity;
    switch (value) {
    case Purity::Pure:
        return "pure";
    case Purity::ReadOnly:
        return "read-only";
    case Purity::Contextual:
        return "contextual";
    case Purity::Impure:
        return "impure";
    }
    return "unknown";
}

std::string_view determinismName(
    mparser::BuiltinDeterminism value) {
    using Determinism = mparser::BuiltinDeterminism;
    switch (value) {
    case Determinism::Deterministic:
        return "deterministic";
    case Determinism::ContextDependent:
        return "context-dependent";
    case Determinism::Nondeterministic:
        return "nondeterministic";
    }
    return "unknown";
}

std::string_view threadSafetyName(
    mparser::BuiltinThreadSafety value) {
    using ThreadSafety = mparser::BuiltinThreadSafety;
    switch (value) {
    case ThreadSafety::Reentrant:
        return "reentrant";
    case ThreadSafety::ContextBound:
        return "context-bound";
    case ThreadSafety::Serialized:
        return "serialized";
    }
    return "unknown";
}

std::string_view valueConstraintName(
    mparser::BuiltinValueConstraint value) {
    using Constraint = mparser::BuiltinValueConstraint;
    switch (value) {
    case Constraint::Any:
        return "any";
    case Constraint::Numeric:
        return "numeric";
    case Constraint::ScalarNumeric:
        return "scalar-numeric";
    case Constraint::Text:
        return "text";
    case Constraint::FunctionHandle:
        return "function-handle";
    }
    return "unknown";
}

std::string_view shapeConstraintName(
    mparser::BuiltinShapeConstraint value) {
    using Constraint = mparser::BuiltinShapeConstraint;
    switch (value) {
    case Constraint::Any:
        return "any";
    case Constraint::Scalar:
        return "scalar";
    case Constraint::DenseArray:
        return "dense-array";
    }
    return "unknown";
}

std::string_view typedLoweringName(
    mparser::BuiltinTypedLowering value) {
    using Lowering = mparser::BuiltinTypedLowering;
    switch (value) {
    case Lowering::None:
        return "none";
    case Lowering::Absolute:
        return "absolute";
    case Lowering::ArcCosine:
        return "arc-cosine";
    case Lowering::ArcSine:
        return "arc-sine";
    case Lowering::ArcTangent:
        return "arc-tangent";
    case Lowering::Cosine:
        return "cosine";
    case Lowering::Exponential:
        return "exponential";
    case Lowering::Logarithm:
        return "logarithm";
    case Lowering::Sine:
        return "sine";
    case Lowering::SquareRoot:
        return "square-root";
    case Lowering::Tangent:
        return "tangent";
    }
    return "unknown";
}

std::string_view implicitOutputPolicyName(
    mparser::BuiltinImplicitOutputPolicy value) {
    using Policy = mparser::BuiltinImplicitOutputPolicy;
    switch (value) {
    case Policy::FirstAvailable:
        return "first-available";
    case Policy::None:
        return "none";
    case Policy::FirstWhenNoArguments:
        return "first-when-no-arguments";
    }
    return "unknown";
}

Json arityJson(const mparser::BuiltinArity& arity) {
    return Json{
        {"minimum", arity.minimum},
        {"maximum", arity.maximum ? Json(*arity.maximum)
                                    : Json(nullptr)},
    };
}

Json constraintsJson(
    const std::vector<mparser::BuiltinArgumentConstraint>&
        constraints) {
    Json result = Json::array();
    for (const auto& constraint : constraints) {
        result.push_back(Json{
            {"value", valueConstraintName(constraint.value)},
            {"shape", shapeConstraintName(constraint.shape)},
        });
    }
    return result;
}

Json sideEffectsJson(mparser::BuiltinSideEffect effects) {
    using Effect = mparser::BuiltinSideEffect;
    Json result = Json::array();
    for (const auto& [effect, name] : {
             std::pair{Effect::Workspace, "workspace"},
             std::pair{Effect::Console, "console"},
             std::pair{Effect::WarningState, "warning-state"},
             std::pair{Effect::Time, "time"},
             std::pair{Effect::ObjectState, "object-state"},
             std::pair{Effect::External, "external"},
             std::pair{Effect::RandomState, "random-state"},
             std::pair{Effect::DisplayState, "display-state"}}) {
        if (mparser::hasBuiltinSideEffect(effects, effect)) {
            result.push_back(name);
        }
    }
    return result;
}

Json permissionsJson(
    mparser::BuiltinContextPermission permissions) {
    using Permission = mparser::BuiltinContextPermission;
    Json result = Json::array();
    for (const auto& [permission, name] : {
             std::pair{Permission::Workspace, "workspace"},
             std::pair{Permission::WarningState, "warning-state"},
             std::pair{Permission::ObjectArrayPolicy,
                       "object-array-policy"},
             std::pair{Permission::DynamicCall, "dynamic-call"},
             std::pair{Permission::ExecutionControl,
                       "execution-control"},
             std::pair{Permission::Output, "output"},
             std::pair{Permission::SystemServices,
                       "system-services"},
             std::pair{Permission::DisplayFormat,
                       "display-format"},
             std::pair{Permission::SourceEvaluation,
                       "source-evaluation"}}) {
        if (mparser::hasBuiltinContextPermission(
                permissions, permission)) {
            result.push_back(name);
        }
    }
    return result;
}

Json descriptorJson(const mparser::BuiltinDescriptor& descriptor) {
    return Json{
        {"name", descriptor.name},
        {"aliases", descriptor.aliases},
        {"inputs", arityJson(descriptor.inputs)},
        {"outputs", arityJson(descriptor.outputs)},
        {"argument_constraints",
         constraintsJson(descriptor.argumentConstraints)},
        {"output_constraints",
         constraintsJson(descriptor.outputConstraints)},
        {"implementation",
         implementationName(descriptor.implementation)},
        {"purity", purityName(descriptor.purity)},
        {"determinism",
         determinismName(descriptor.determinism)},
        {"thread_safety",
         threadSafetyName(descriptor.threadSafety)},
        {"side_effects", sideEffectsJson(descriptor.sideEffects)},
        {"context_permissions",
         permissionsJson(descriptor.contextPermissions)},
        {"required_context",
         permissionsJson(descriptor.requiredContext)},
        {"typed_lowering",
         typedLoweringName(descriptor.typedLowering)},
        {"implicit_output_policy",
         implicitOutputPolicyName(descriptor.implicitOutputPolicy)},
        {"error_identifier", descriptor.errorIdentifier},
    };
}

Json catalogJson() {
    const auto registry = mparser::defaultBuiltinRegistry();
    Json descriptors = Json::array();
    for (const auto& descriptor : registry->descriptors()) {
        descriptors.push_back(descriptorJson(descriptor.get()));
    }
    return Json{
        {"schema",
         {{"name", "mparser.builtin-catalog"},
          {"major", 1},
          {"minor", 1}}},
        {"source_contract",
         {{"major", mparser::kBuiltinSourceContractMajor},
          {"minor", mparser::kBuiltinSourceContractMinor}}},
        {"catalog", "default"},
        {"descriptor_count", descriptors.size()},
        {"descriptors", std::move(descriptors)},
    };
}

Json readJson(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            std::string("failed to open builtin catalog: ") + path);
    }
    Json value;
    input >> value;
    return value;
}

void writeJson(const char* path, const Json& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            std::string("failed to write builtin catalog: ") + path);
    }
    output << value.dump(2) << '\n';
    if (!output) {
        throw std::runtime_error(
            std::string("failed to finish builtin catalog: ") + path);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Json actual = catalogJson();
        if (argc == 3 && std::string_view(argv[1]) == "--write") {
            writeJson(argv[2], actual);
            std::cout << "wrote builtin catalog source-contract snapshot "
                      << argv[2] << '\n';
            return 0;
        }
        if (argc != 2) {
            throw std::runtime_error(
                "usage: builtin_catalog_snapshot "
                "[--write] <default_catalog.json>");
        }
        const Json expected = readJson(argv[1]);
        if (actual != expected) {
            throw std::runtime_error(
                "default builtin catalog differs from the frozen "
                "source-contract snapshot");
        }
        std::cout << "builtin source contract "
                  << mparser::kBuiltinSourceContractMajor << '.'
                  << mparser::kBuiltinSourceContractMinor
                  << " catalog validated: "
                  << actual.at("descriptor_count") << " descriptors\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Builtin catalog snapshot failure: "
                  << error.what() << '\n';
        return 1;
    }
}
