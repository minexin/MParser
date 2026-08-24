#include "mparser/execution/jit/dense_array_region_executor.h"

#include "mparser/execution/jit/native_scalar_jit.h"
#include "mparser/execution/jit/typed_scalar_kernel.h"
#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/builtins/numeric/runtime_reduction.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser {
namespace {

enum class DenseNodeKind {
    Input,
    Literal,
    Unary,
    Binary,
};

struct DenseNode {
    DenseNodeKind kind = DenseNodeKind::Literal;
    ScalarKernelOp operation = ScalarKernelOp::Copy;
    size_t left = 0;
    size_t right = 0;
    std::string input;
    RuntimeNumericElementValue literal;
    bool matrixOperator = false;
};

enum class DenseReductionSelection {
    None,
    Default,
    All,
    Dimension,
};

struct ParsedDenseRegion {
    DenseArrayRegionAnalysis analysis;
    std::vector<DenseNode> nodes;
    size_t resultNode = 0;
    DenseReductionSelection reduction = DenseReductionSelection::None;
    size_t reductionDimension = 0;
    BuiltinTypedLowering reductionLowering =
        BuiltinTypedLowering::None;
};

std::string_view reductionName(BuiltinTypedLowering lowering) {
    switch (lowering) {
    case BuiltinTypedLowering::Sum:
        return "sum";
    case BuiltinTypedLowering::Product:
        return "prod";
    case BuiltinTypedLowering::Mean:
        return "mean";
    default:
        return "reduction";
    }
}

enum class ParseStackKind {
    Node,
    Callable,
    AllOption,
};

struct ParseStackValue {
    ParseStackKind kind = ParseStackKind::Node;
    size_t node = 0;
    std::string callable;
};

bool isDenseVariableBinding(BindingKind kind) {
    return kind == BindingKind::LocalVariable ||
           kind == BindingKind::FunctionParameter ||
           kind == BindingKind::FunctionOutput;
}

std::optional<ScalarKernelOp> unaryOperation(std::string_view operation) {
    if (operation == "+") {
        return ScalarKernelOp::UnaryPlus;
    }
    if (operation == "-") {
        return ScalarKernelOp::UnaryMinus;
    }
    return std::nullopt;
}

std::optional<ScalarKernelOp> binaryOperation(
    std::string_view operation) {
    if (operation == "+") {
        return ScalarKernelOp::Add;
    }
    if (operation == "-") {
        return ScalarKernelOp::Subtract;
    }
    if (operation == ".*" || operation == "*") {
        return ScalarKernelOp::Multiply;
    }
    if (operation == "./" || operation == "/") {
        return ScalarKernelOp::Divide;
    }
    if (operation == ".^" || operation == "^") {
        return ScalarKernelOp::Power;
    }
    return std::nullopt;
}

std::optional<ScalarKernelOp> builtinOperation(
    BuiltinTypedLowering lowering) {
    switch (lowering) {
    case BuiltinTypedLowering::Absolute:
        return ScalarKernelOp::Absolute;
    case BuiltinTypedLowering::ArcCosine:
        return ScalarKernelOp::ArcCosine;
    case BuiltinTypedLowering::ArcSine:
        return ScalarKernelOp::ArcSine;
    case BuiltinTypedLowering::ArcTangent:
        return ScalarKernelOp::ArcTangent;
    case BuiltinTypedLowering::Cosine:
        return ScalarKernelOp::Cosine;
    case BuiltinTypedLowering::Exponential:
        return ScalarKernelOp::Exponential;
    case BuiltinTypedLowering::Logarithm:
        return ScalarKernelOp::Logarithm;
    case BuiltinTypedLowering::Sine:
        return ScalarKernelOp::Sine;
    case BuiltinTypedLowering::SquareRoot:
        return ScalarKernelOp::SquareRoot;
    case BuiltinTypedLowering::Tangent:
        return ScalarKernelOp::Tangent;
    case BuiltinTypedLowering::None:
    case BuiltinTypedLowering::Sum:
    case BuiltinTypedLowering::Product:
    case BuiltinTypedLowering::Mean:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<int> expressionStackDelta(
    const BytecodeInstruction& instruction) {
    switch (instruction.op) {
    case BytecodeOp::LoadName:
        return instruction.resultCount == 1
                   ? std::optional<int>(1)
                   : std::nullopt;
    case BytecodeOp::LoadLiteral:
        return 1;
    case BytecodeOp::UnaryOp:
    case BytecodeOp::PostfixOp:
        return 0;
    case BytecodeOp::BinaryOp:
        return instruction.operandCount == 2
                   ? std::optional<int>(-1)
                   : std::nullopt;
    case BytecodeOp::CallOrIndex:
        if (instruction.operandCount < 0 || instruction.resultCount != 1) {
            return std::nullopt;
        }
        return -instruction.operandCount;
    default:
        return std::nullopt;
    }
}

std::optional<size_t> expressionBegin(
    const BytecodeProgram& program, size_t storePc) {
    int delta = 0;
    for (size_t pc = storePc; pc-- > 0;) {
        const auto effect = expressionStackDelta(program.instructions[pc]);
        if (!effect) {
            return std::nullopt;
        }
        delta += *effect;
        if (delta == 1) {
            return pc;
        }
    }
    return std::nullopt;
}

std::optional<RuntimeNumericElementValue> numericLiteral(
    std::string_view text) {
    const auto value = runtimeParseNumericLiteral(text);
    if (!value || value->kind != RuntimeValueKind::Number ||
        !runtimeNumericClassIsFloating(value->numericClass)) {
        return std::nullopt;
    }
    return runtimeNumericElementValue(*value, 0);
}

std::optional<RuntimeNumericElementValue> numericBuiltinConstant(
    std::string_view name) {
    if (name == "i" || name == "j") {
        return numericLiteral("1i");
    }
    if (name == "pi") {
        return numericLiteral("3.141592653589793238462643383279502884");
    }
    if (name == "eps") {
        return numericLiteral("2.220446049250313080847263336181640625e-16");
    }
    if (name == "inf") {
        RuntimeNumericElementValue value;
        value.real = std::numeric_limits<double>::infinity();
        return value;
    }
    if (name == "nan") {
        RuntimeNumericElementValue value;
        value.real = std::numeric_limits<double>::quiet_NaN();
        return value;
    }
    return std::nullopt;
}

bool isAllLiteral(std::string_view text) {
    return text == "\"all\"" || text == "'all'";
}

RuntimeFallbackKind parseFallbackKind(
    const DenseArrayRegionAnalysis& analysis) {
    if (!analysis.available) {
        return RuntimeFallbackKind::RegionUnavailable;
    }
    if (!analysis.closed) {
        return RuntimeFallbackKind::RegionNotClosed;
    }
    if (analysis.hasUnsupportedCall) {
        return RuntimeFallbackKind::ContainsCall;
    }
    return RuntimeFallbackKind::UnsupportedOperation;
}

ParsedDenseRegion parseDenseRegion(
    const BytecodeProgram& program, size_t storePc,
    const BuiltinRegistry& builtinRegistry) {
    ParsedDenseRegion parsed;
    auto& analysis = parsed.analysis;
    analysis.storePc = storePc;
    analysis.endPc = storePc + 1;

    const auto reject = [&](std::string reason, bool call = false) {
        analysis.hasUnsupportedCall =
            analysis.hasUnsupportedCall || call;
        analysis.hasUnsupportedOperation =
            analysis.hasUnsupportedOperation || !call;
        analysis.fallbackKind = parseFallbackKind(analysis);
        analysis.reason = std::move(reason);
        return parsed;
    };

    if (storePc >= program.instructions.size()) {
        analysis.reason = "dense assignment store is out of range";
        analysis.fallbackKind = RuntimeFallbackKind::RegionUnavailable;
        return parsed;
    }
    const auto& store = program.instructions[storePc];
    if (store.op != BytecodeOp::StoreName ||
        !isDenseVariableBinding(store.binding.kind) ||
        store.operand.empty()) {
        analysis.reason = "candidate is not a local name assignment";
        analysis.fallbackKind = RuntimeFallbackKind::RegionUnavailable;
        return parsed;
    }
    analysis.target = store.operand;
    const auto begin = expressionBegin(program, storePc);
    if (!begin || *begin >= storePc) {
        analysis.reason = "assignment right-hand side is not a closed stack expression";
        analysis.fallbackKind = RuntimeFallbackKind::RegionNotClosed;
        return parsed;
    }
    analysis.available = true;
    analysis.beginPc = *begin;

    std::set<std::string> inputs;
    std::set<std::string> calls;
    std::vector<ParseStackValue> stack;
    for (size_t pc = *begin; pc < storePc; ++pc) {
        const auto& instruction = program.instructions[pc];
        switch (instruction.op) {
        case BytecodeOp::LoadName: {
            if (instruction.binding.kind == BindingKind::Builtin) {
                if (const auto constant =
                        numericBuiltinConstant(instruction.operand)) {
                    DenseNode node;
                    node.kind = DenseNodeKind::Literal;
                    node.literal = *constant;
                    parsed.nodes.push_back(std::move(node));
                    stack.push_back(ParseStackValue{
                        ParseStackKind::Node,
                        parsed.nodes.size() - 1, {}});
                    break;
                }
                const BuiltinDescriptor* descriptor =
                    builtinRegistry.find(instruction.operand);
                if (!descriptor || descriptor->purity != BuiltinPurity::Pure ||
                    descriptor->determinism !=
                        BuiltinDeterminism::Deterministic ||
                    descriptor->sideEffects != BuiltinSideEffect::None ||
                    descriptor->typedLowering ==
                        BuiltinTypedLowering::None) {
                    return reject(
                        "dense expression references an unsupported callable",
                        true);
                }
                stack.push_back(ParseStackValue{
                    ParseStackKind::Callable, 0,
                    instruction.operand});
                calls.insert(instruction.operand);
                break;
            }
            if (!isDenseVariableBinding(instruction.binding.kind) ||
                instruction.operand.empty()) {
                return reject(
                    "dense expression reads a nonlocal binding");
            }
            DenseNode node;
            node.kind = DenseNodeKind::Input;
            node.input = instruction.operand;
            parsed.nodes.push_back(std::move(node));
            stack.push_back(ParseStackValue{
                ParseStackKind::Node, parsed.nodes.size() - 1, {}});
            inputs.insert(instruction.operand);
            break;
        }
        case BytecodeOp::LoadLiteral: {
            if (isAllLiteral(instruction.operand)) {
                stack.push_back(ParseStackValue{
                    ParseStackKind::AllOption, 0, {}});
                break;
            }
            const auto value = numericLiteral(instruction.operand);
            if (!value) {
                return reject(
                    "dense expression contains a non-floating literal");
            }
            DenseNode node;
            node.kind = DenseNodeKind::Literal;
            node.literal = *value;
            parsed.nodes.push_back(std::move(node));
            stack.push_back(ParseStackValue{
                ParseStackKind::Node, parsed.nodes.size() - 1, {}});
            break;
        }
        case BytecodeOp::UnaryOp: {
            const auto operation = unaryOperation(instruction.operand);
            if (!operation || stack.empty() ||
                stack.back().kind != ParseStackKind::Node) {
                return reject(
                    "dense expression contains an unsupported unary operation");
            }
            DenseNode node;
            node.kind = DenseNodeKind::Unary;
            node.operation = *operation;
            node.left = stack.back().node;
            parsed.nodes.push_back(std::move(node));
            stack.back().node = parsed.nodes.size() - 1;
            ++analysis.elementwiseOperationCount;
            break;
        }
        case BytecodeOp::BinaryOp: {
            const auto operation = binaryOperation(instruction.operand);
            if (!operation || stack.size() < 2 ||
                stack[stack.size() - 2].kind != ParseStackKind::Node ||
                stack.back().kind != ParseStackKind::Node) {
                return reject(
                    "dense expression contains an unsupported binary operation");
            }
            const size_t right = stack.back().node;
            stack.pop_back();
            const size_t left = stack.back().node;
            DenseNode node;
            node.kind = DenseNodeKind::Binary;
            node.operation = *operation;
            node.left = left;
            node.right = right;
            node.matrixOperator =
                instruction.operand == "*" ||
                instruction.operand == "/" ||
                instruction.operand == "^";
            analysis.explicitArraySyntax =
                analysis.explicitArraySyntax ||
                instruction.operand == ".*" ||
                instruction.operand == "./" ||
                instruction.operand == ".^";
            parsed.nodes.push_back(std::move(node));
            stack.back().node = parsed.nodes.size() - 1;
            ++analysis.elementwiseOperationCount;
            break;
        }
        case BytecodeOp::CallOrIndex: {
            if (instruction.operandCount < 0 ||
                instruction.resultCount != 1 ||
                instruction.implicitExpressionOutput) {
                return reject("dense expression call contract is unsupported",
                              true);
            }
            const size_t argumentCount =
                static_cast<size_t>(instruction.operandCount);
            if (stack.size() < argumentCount + 1) {
                return reject("dense expression call stack underflows", true);
            }
            const size_t callableIndex =
                stack.size() - argumentCount - 1;
            const auto callable = stack[callableIndex];
            if (callable.kind != ParseStackKind::Callable ||
                callable.callable != instruction.calleeName) {
                return reject(
                    "dense expression callable identity is not closed",
                    true);
            }
            const BuiltinDescriptor* descriptor =
                builtinRegistry.find(callable.callable);
            if (!descriptor) {
                return reject("dense expression builtin is unavailable", true);
            }
            if (builtinTypedLoweringIsElementwiseUnary(
                    descriptor->typedLowering)) {
                if (argumentCount != 1 ||
                    stack[callableIndex + 1].kind !=
                        ParseStackKind::Node) {
                    return reject(
                        "typed element-wise builtin requires one numeric argument",
                        true);
                }
                const auto operation =
                    builtinOperation(descriptor->typedLowering);
                if (!operation) {
                    return reject(
                        "typed element-wise builtin lowering is unavailable",
                        true);
                }
                DenseNode node;
                node.kind = DenseNodeKind::Unary;
                node.operation = *operation;
                node.left = stack[callableIndex + 1].node;
                parsed.nodes.push_back(std::move(node));
                stack.resize(callableIndex);
                stack.push_back(ParseStackValue{
                    ParseStackKind::Node, parsed.nodes.size() - 1, {}});
                ++analysis.elementwiseOperationCount;
                analysis.explicitArraySyntax = true;
                break;
            }
            if (builtinTypedLoweringIsReduction(
                    descriptor->typedLowering)) {
                if (analysis.reductionOperationCount != 0 ||
                    pc + 1 != storePc ||
                    (argumentCount != 1 && argumentCount != 2) ||
                    stack[callableIndex + 1].kind !=
                        ParseStackKind::Node) {
                    return reject(
                        "typed reduction must terminate one dense assignment",
                        true);
                }
                parsed.reductionLowering = descriptor->typedLowering;
                parsed.reduction = DenseReductionSelection::Default;
                if (argumentCount == 2) {
                    const auto& option = stack[callableIndex + 2];
                    if (option.kind == ParseStackKind::AllOption) {
                        parsed.reduction = DenseReductionSelection::All;
                    } else if (option.kind == ParseStackKind::Node) {
                        const auto& optionNode = parsed.nodes[option.node];
                        const auto dimension =
                            optionNode.kind == DenseNodeKind::Literal &&
                                    !optionNode.literal.complex
                                ? checkedRuntimeNonnegativeInteger(
                                      optionNode.literal.real)
                                : std::nullopt;
                        if (!dimension || *dimension == 0) {
                            return reject(
                                "typed reduction dimension must be a positive integer literal",
                                true);
                        }
                        parsed.reduction =
                            DenseReductionSelection::Dimension;
                        parsed.reductionDimension = *dimension - 1;
                    } else {
                        return reject(
                            "typed reduction option must be a dimension or \"all\"",
                            true);
                    }
                }
                parsed.resultNode = stack[callableIndex + 1].node;
                stack.resize(callableIndex);
                stack.push_back(ParseStackValue{
                    ParseStackKind::Node, parsed.resultNode, {}});
                ++analysis.reductionOperationCount;
                analysis.explicitArraySyntax = true;
                break;
            }
            return reject("dense expression builtin has no supported lowering",
                          true);
        }
        default:
            return reject(
                "dense expression contains unsupported bytecode");
        }
    }

    if (stack.size() != 1 ||
        stack.front().kind != ParseStackKind::Node) {
        return reject(
            "dense assignment does not restore its stack boundary");
    }
    if (analysis.reductionOperationCount == 0) {
        parsed.resultNode = stack.front().node;
    }
    analysis.closed = true;
    analysis.eligible = true;
    analysis.inputs.assign(inputs.begin(), inputs.end());
    analysis.callTargets.assign(calls.begin(), calls.end());
    analysis.fallbackKind = RuntimeFallbackKind::None;
    analysis.reason = analysis.reductionOperationCount == 0
                          ? "eligible closed dense element-wise assignment"
                          : "eligible closed fused dense " +
                                std::string(reductionName(
                                    parsed.reductionLowering)) +
                                " assignment";
    return parsed;
}

struct RuntimeDenseInput {
    std::string name;
    const RuntimeValue* value = nullptr;
    bool scalar = false;
    std::vector<size_t> dimensions;
    std::vector<size_t> rowMajorStrides;
};

struct PreparedDenseNode {
    DenseNode node;
    size_t inputIndex = 0;
    std::vector<size_t> dimensions;
};

struct PreparedDenseRegion {
    ParsedDenseRegion parsed;
    std::vector<RuntimeDenseInput> inputs;
    std::vector<PreparedDenseNode> nodes;
    std::vector<size_t> sourceDimensions;
    std::vector<size_t> workingDimensions;
    std::vector<size_t> outputDimensions;
    std::vector<size_t> reducedDimensions;
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
    size_t sourceElementCount = 0;
    size_t outputElementCount = 0;
};

DenseArrayRegionExecutionResult executionFallback(
    RuntimeFallbackKind kind, std::string reason) {
    DenseArrayRegionExecutionResult result;
    result.fallbackKind = kind;
    result.reason = std::move(reason);
    return result;
}

std::vector<size_t> rowMajorStrides(
    const std::vector<size_t>& dimensions) {
    std::vector<size_t> strides(dimensions.size(), 1);
    for (size_t index = dimensions.size(); index > 1; --index) {
        strides[index - 2] =
            strides[index - 1] * dimensions[index - 1];
    }
    return strides;
}

std::optional<PreparedDenseRegion> prepareDenseRegion(
    const BytecodeProgram& program, const BytecodeRegionContract& region,
    const RuntimeWorkspace& variables, const BuiltinRegistry& registry,
    std::string& failureReason) {
    auto parsed = parseDenseRegion(program, region.bodyEndPc, registry);
    if (!parsed.analysis.eligible ||
        parsed.analysis.beginPc != region.beginPc ||
        parsed.analysis.endPc != region.endPc ||
        region.outputs.size() != 1 ||
        parsed.analysis.target != region.outputs.front()) {
        failureReason = "dense runtime region does not match its bytecode contract";
        return std::nullopt;
    }
    for (const auto& callable : parsed.analysis.callTargets) {
        if (variables.contains(callable)) {
            failureReason =
                "typed dense callable is shadowed by workspace variable: " +
                callable;
            return std::nullopt;
        }
    }

    PreparedDenseRegion prepared;
    prepared.parsed = std::move(parsed);
    std::map<std::string, size_t, std::less<>> inputIndices;
    bool hasArrayInput = false;
    for (const auto& name : prepared.parsed.analysis.inputs) {
        const auto found = variables.find(name);
        if (found == variables.end()) {
            failureReason = "typed dense input is unavailable: " + name;
            return std::nullopt;
        }
        const RuntimeValue& value = found->second;
        RuntimeDenseInput input;
        input.name = name;
        input.value = &value;
        input.dimensions = runtimeDimensions(value);
        if (!isRuntimeNumericValue(value) ||
            !runtimeNumericClassIsFloating(value.numericClass)) {
            failureReason =
                "typed dense input is not a single or double numeric value: " +
                name;
            return std::nullopt;
        }
        if (value.numericClass == RuntimeNumericClass::Single) {
            prepared.numericClass = RuntimeNumericClass::Single;
        }
        if (value.kind == RuntimeValueKind::Number) {
            input.scalar = true;
        } else if ((value.kind == RuntimeValueKind::Vector ||
                    value.kind == RuntimeValueKind::Matrix)) {
            const auto count =
                checkedRuntimeDimensionProduct(input.dimensions);
            if (!count || *count != value.elements.size() ||
                (value.numericComplex &&
                 value.imaginaryElements.size() != *count)) {
                failureReason =
                    "typed dense input has inconsistent shape storage: " +
                    name;
                return std::nullopt;
            }
            input.rowMajorStrides =
                rowMajorStrides(input.dimensions);
            hasArrayInput = true;
        } else {
            failureReason =
                "typed dense input is not a dense scalar or array: " +
                name;
            return std::nullopt;
        }
        inputIndices.emplace(name, prepared.inputs.size());
        prepared.inputs.push_back(std::move(input));
    }

    prepared.nodes.reserve(prepared.parsed.nodes.size());
    for (const auto& node : prepared.parsed.nodes) {
        PreparedDenseNode preparedNode;
        preparedNode.node = node;
        switch (node.kind) {
        case DenseNodeKind::Input: {
            const auto input = inputIndices.find(node.input);
            if (input == inputIndices.end()) {
                failureReason =
                    "typed dense node references an unknown input";
                return std::nullopt;
            }
            preparedNode.inputIndex = input->second;
            preparedNode.dimensions =
                prepared.inputs[input->second].dimensions;
            break;
        }
        case DenseNodeKind::Literal:
            preparedNode.dimensions = {1, 1};
            break;
        case DenseNodeKind::Unary:
            if (node.left >= prepared.nodes.size()) {
                failureReason =
                    "typed dense unary node has an invalid operand";
                return std::nullopt;
            }
            preparedNode.dimensions =
                prepared.nodes[node.left].dimensions;
            break;
        case DenseNodeKind::Binary: {
            if (node.left >= prepared.nodes.size() ||
                node.right >= prepared.nodes.size()) {
                failureReason =
                    "typed dense binary node has an invalid operand";
                return std::nullopt;
            }
            const auto leftCount = checkedRuntimeDimensionProduct(
                prepared.nodes[node.left].dimensions);
            const auto rightCount = checkedRuntimeDimensionProduct(
                prepared.nodes[node.right].dimensions);
            if (!leftCount || !rightCount) {
                failureReason =
                    "typed dense operand shape is too large";
                return std::nullopt;
            }
            const bool matrixOperationSupported =
                !node.matrixOperator ||
                (node.operation == ScalarKernelOp::Multiply &&
                 (*leftCount == 1 || *rightCount == 1)) ||
                (node.operation == ScalarKernelOp::Divide &&
                 *rightCount == 1) ||
                (node.operation == ScalarKernelOp::Power &&
                 *leftCount == 1 && *rightCount == 1);
            if (!matrixOperationSupported) {
                failureReason =
                    "matrix multiply, divide, and power remain in the VM for nonscalar operands";
                return std::nullopt;
            }
            const auto dimensions = runtimeImplicitExpansionDimensions(
                prepared.nodes[node.left].dimensions,
                prepared.nodes[node.right].dimensions);
            if (!dimensions) {
                failureReason =
                    "typed dense operands have incompatible implicit-expansion shapes";
                return std::nullopt;
            }
            preparedNode.dimensions = *dimensions;
            break;
        }
        }
        prepared.nodes.push_back(std::move(preparedNode));
    }

    if (prepared.parsed.resultNode >= prepared.nodes.size()) {
        failureReason = "typed dense result node is invalid";
        return std::nullopt;
    }
    prepared.sourceDimensions =
        prepared.nodes[prepared.parsed.resultNode].dimensions;
    const auto sourceCount =
        checkedRuntimeDimensionProduct(prepared.sourceDimensions);
    if (!sourceCount) {
        failureReason = "typed dense source shape is too large";
        return std::nullopt;
    }
    prepared.sourceElementCount = *sourceCount;
    if (!hasArrayInput) {
        failureReason =
            "typed dense region requires at least one dense array input";
        return std::nullopt;
    }

    prepared.workingDimensions = prepared.sourceDimensions;
    prepared.outputDimensions = prepared.sourceDimensions;
    const auto selection = prepared.parsed.reduction;
    if (selection != DenseReductionSelection::None) {
        std::vector<size_t> dimensions;
        if (selection == DenseReductionSelection::Dimension) {
            dimensions = {prepared.parsed.reductionDimension};
        }
        auto shape = runtimeReductionShape(
            prepared.sourceDimensions,
            selection != DenseReductionSelection::Default,
            selection == DenseReductionSelection::All,
            std::move(dimensions), false);
        prepared.workingDimensions =
            std::move(shape.inputDimensions);
        prepared.reducedDimensions =
            std::move(shape.reductionDimensions);
        prepared.outputDimensions =
            std::move(shape.outputDimensions);
        prepared.outputDimensions = normalizeRuntimeDimensions(
            prepared.outputDimensions);
    }
    const auto outputCount =
        checkedRuntimeDimensionProduct(prepared.outputDimensions);
    if (!outputCount) {
        failureReason = "typed dense output shape is too large";
        return std::nullopt;
    }
    prepared.outputElementCount = *outputCount;
    return prepared;
}

std::optional<size_t> inputStorageOffset(
    const RuntimeDenseInput& input,
    const std::vector<size_t>& coordinates) {
    if (input.scalar) {
        return 0;
    }
    if (coordinates.size() < input.dimensions.size()) {
        return std::nullopt;
    }
    size_t offset = 0;
    for (size_t index = 0; index < input.dimensions.size(); ++index) {
        const size_t coordinate =
            input.dimensions[index] == 1 ? 0 : coordinates[index];
        if (coordinate >= input.dimensions[index] ||
            coordinate >
                (std::numeric_limits<size_t>::max() - offset) /
                    input.rowMajorStrides[index]) {
            return std::nullopt;
        }
        offset += coordinate * input.rowMajorStrides[index];
    }
    return offset;
}

std::optional<RuntimeNumericElementValue> evaluateUnary(
    ScalarKernelOp operation, const RuntimeNumericElementValue& input,
    RuntimeNumericClass resultClass, std::string& failureReason) {
    const auto converted = runtimeConvertNumericElementValue(
        input, resultClass);
    if (!converted) {
        failureReason = "typed dense unary input could not be represented";
        return std::nullopt;
    }

    if (operation == ScalarKernelOp::Copy ||
        operation == ScalarKernelOp::UnaryPlus) {
        return converted;
    }
    if (operation == ScalarKernelOp::UnaryMinus) {
        RuntimeNumericElementValue result = *converted;
        result.real = -result.real;
        if (result.complex) {
            result.imaginary = -result.imaginary;
        }
        return result;
    }

    const std::complex<double> complexInput(
        converted->real,
        converted->complex ? converted->imaginary : 0.0);
    RuntimeNumericElementValue result;
    result.numericClass = resultClass;
    switch (operation) {
    case ScalarKernelOp::Absolute:
        result.real = std::abs(complexInput);
        result.complex = false;
        break;
    case ScalarKernelOp::ArcCosine:
        if (!converted->complex && std::fabs(converted->real) <= 1.0) {
            result.real = std::acos(converted->real);
            result.complex = false;
        } else {
            const auto raw = std::acos(complexInput);
            result.real = raw.real();
            result.imaginary = raw.imag();
            if (!converted->complex &&
                std::fabs(converted->real) > 1.0) {
                result.imaginary = std::copysign(
                    std::fabs(result.imaginary), converted->real);
            }
            result.complex = converted->complex || result.imaginary != 0.0;
        }
        break;
    case ScalarKernelOp::ArcSine:
        if (!converted->complex && std::fabs(converted->real) <= 1.0) {
            result.real = std::asin(converted->real);
            result.complex = false;
        } else {
            const auto raw = std::asin(complexInput);
            result.real = raw.real();
            result.imaginary = raw.imag();
            if (!converted->complex &&
                std::fabs(converted->real) > 1.0) {
                result.imaginary = std::copysign(
                    std::fabs(result.imaginary), -converted->real);
            }
            result.complex = converted->complex || result.imaginary != 0.0;
        }
        break;
    case ScalarKernelOp::ArcTangent:
        if (!converted->complex) {
            result.real = std::atan(converted->real);
            result.complex = false;
        } else {
            const auto raw = std::atan(complexInput);
            result.real = raw.real();
            result.imaginary = raw.imag();
            result.complex = true;
        }
        break;
    case ScalarKernelOp::Cosine: {
        const auto raw = converted->complex
                             ? std::cos(complexInput)
                             : std::complex<double>{
                                   std::cos(converted->real), 0.0};
        result.real = raw.real();
        result.imaginary = raw.imag();
        result.complex = converted->complex || result.imaginary != 0.0;
        break;
    }
    case ScalarKernelOp::Exponential: {
        const auto raw = converted->complex
                             ? std::exp(complexInput)
                             : std::complex<double>{
                                   std::exp(converted->real), 0.0};
        result.real = raw.real();
        result.imaginary = raw.imag();
        result.complex = converted->complex || result.imaginary != 0.0;
        break;
    }
    case ScalarKernelOp::Logarithm:
        if (!converted->complex && converted->real >= 0.0) {
            result.real = std::log(converted->real);
            result.complex = false;
        } else {
            const auto raw = std::log(complexInput);
            result.real = raw.real();
            result.imaginary = raw.imag();
            result.complex = converted->complex || result.imaginary != 0.0;
        }
        break;
    case ScalarKernelOp::Sine: {
        const auto raw = converted->complex
                             ? std::sin(complexInput)
                             : std::complex<double>{
                                   std::sin(converted->real), 0.0};
        result.real = raw.real();
        result.imaginary = raw.imag();
        result.complex = converted->complex || result.imaginary != 0.0;
        break;
    }
    case ScalarKernelOp::SquareRoot:
        if (!converted->complex && converted->real >= 0.0) {
            result.real = std::sqrt(converted->real);
            result.complex = false;
        } else {
            const auto raw = std::sqrt(complexInput);
            result.real = raw.real();
            result.imaginary = raw.imag();
            result.complex = converted->complex || result.imaginary != 0.0;
        }
        break;
    case ScalarKernelOp::Tangent: {
        const auto raw = converted->complex
                             ? std::tan(complexInput)
                             : std::complex<double>{
                                   std::tan(converted->real), 0.0};
        result.real = raw.real();
        result.imaginary = raw.imag();
        result.complex = converted->complex || result.imaginary != 0.0;
        break;
    }
    default:
        failureReason = "typed dense unary operation is unsupported";
        return std::nullopt;
    }
    const auto convertedResult = runtimeConvertNumericElementValue(
        result, resultClass);
    if (!convertedResult) {
        failureReason = "typed dense unary result could not be represented";
    }
    return convertedResult;
}

std::optional<RuntimeNumericElementValue> evaluateBinary(
    ScalarKernelOp operation, const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right, RuntimeNumericClass resultClass,
    std::string& failureReason) {
    std::string_view operationName;
    switch (operation) {
    case ScalarKernelOp::Add:
        operationName = "+";
        break;
    case ScalarKernelOp::Subtract:
        operationName = "-";
        break;
    case ScalarKernelOp::Multiply:
        operationName = "*";
        break;
    case ScalarKernelOp::Divide:
        operationName = "/";
        break;
    case ScalarKernelOp::Power:
        operationName = "^";
        break;
    default:
        failureReason = "typed dense binary operation is unsupported";
        return std::nullopt;
    }
    const auto result = runtimeApplyNumericElementBinary(
        operationName, left, right, resultClass);
    if (!result) {
        failureReason = "typed dense binary result could not be represented";
    }
    return result;
}

bool evaluatePreparedNodes(
    const PreparedDenseRegion& prepared,
    const std::vector<size_t>& coordinates,
    std::vector<RuntimeNumericElementValue>& registers,
    RuntimeNumericElementValue& result, std::string& failureReason) {
    for (size_t index = 0; index < prepared.nodes.size(); ++index) {
        const auto& node = prepared.nodes[index];
        switch (node.node.kind) {
        case DenseNodeKind::Input: {
            const auto& input = prepared.inputs[node.inputIndex];
            const auto offset = input.scalar
                                    ? std::optional<size_t>(0)
                                    : inputStorageOffset(input, coordinates);
            if (!offset) {
                failureReason =
                    "typed dense input offset is outside its guarded shape";
                return false;
            }
            const auto element = runtimeNumericStorageElementValue(
                *input.value, *offset);
            const auto converted = element
                                       ? runtimeConvertNumericElementValue(
                                             *element, prepared.numericClass)
                                       : std::nullopt;
            if (!converted) {
                failureReason =
                    "typed dense input element could not be represented";
                return false;
            }
            registers[index] = *converted;
            break;
        }
        case DenseNodeKind::Literal:
            if (const auto converted =
                    runtimeConvertNumericElementValue(
                        node.node.literal, prepared.numericClass)) {
                registers[index] = *converted;
            } else {
                failureReason =
                    "typed dense literal could not be represented";
                return false;
            }
            break;
        case DenseNodeKind::Unary:
            if (const auto value = evaluateUnary(
                    node.node.operation, registers[node.node.left],
                    prepared.numericClass, failureReason)) {
                registers[index] = *value;
            } else {
                return false;
            }
            break;
        case DenseNodeKind::Binary:
            if (const auto value = evaluateBinary(
                    node.node.operation, registers[node.node.left],
                    registers[node.node.right], prepared.numericClass,
                    failureReason)) {
                registers[index] = *value;
            } else {
                return false;
            }
            break;
        }
    }
    result = registers[prepared.parsed.resultNode];
    return true;
}

void incrementColumnMajorCoordinates(
    std::vector<size_t>& coordinates,
    const std::vector<size_t>& dimensions) {
    for (size_t index = 0; index < dimensions.size(); ++index) {
        ++coordinates[index];
        if (coordinates[index] < dimensions[index]) {
            return;
        }
        coordinates[index] = 0;
    }
}

std::optional<size_t> outputBucketIndex(
    const PreparedDenseRegion& prepared,
    const std::vector<size_t>& sourceCoordinates) {
    if (prepared.parsed.reduction == DenseReductionSelection::All) {
        return 0;
    }
    std::vector<size_t> coordinates = sourceCoordinates;
    coordinates.resize(prepared.workingDimensions.size(), 0);
    auto outputDimensions = prepared.workingDimensions;
    for (const size_t dimension : prepared.reducedDimensions) {
        coordinates[dimension] = 0;
        outputDimensions[dimension] = 1;
    }
    return runtimeColumnMajorLinearIndex(coordinates, outputDimensions);
}

std::optional<RuntimeValue> numericValueFromStorage(
    std::vector<size_t> dimensions,
    const std::vector<RuntimeNumericElementValue>& storage,
    RuntimeNumericClass numericClass) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || *count != storage.size()) {
        return std::nullopt;
    }
    std::vector<RuntimeNumericElementValue> logical;
    logical.reserve(storage.size());
    for (size_t logicalIndex = 0; logicalIndex < *count;
         ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, dimensions);
        const auto storageOffset = coordinates
                                       ? runtimeRowMajorStorageOffset(
                                             *coordinates, dimensions)
                                       : std::nullopt;
        if (!storageOffset || *storageOffset >= storage.size()) {
            return std::nullopt;
        }
        logical.push_back(storage[*storageOffset]);
    }
    return runtimeNumericValueFromElements(
        std::move(dimensions), std::move(logical), numericClass);
}

std::optional<RuntimeValue> numericValueFromStorage(
    std::vector<size_t> dimensions, const std::vector<double>& storage,
    RuntimeNumericClass numericClass) {
    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(storage.size());
    for (const double value : storage) {
        RuntimeNumericElementValue element;
        element.numericClass = RuntimeNumericClass::Double;
        element.real = value;
        elements.push_back(element);
    }
    return numericValueFromStorage(
        std::move(dimensions), elements, numericClass);
}

DenseArrayRegionExecutionResult executePortable(
    const PreparedDenseRegion& prepared,
    const BytecodeRegionContract& region) {
    std::vector<RuntimeNumericElementValue> registers(
        prepared.nodes.size());
    std::vector<size_t> coordinates(
        prepared.sourceDimensions.size(), 0);
    std::string failureReason;
    RuntimeValue output;
    size_t kernelInstructions = 0;

    if (prepared.parsed.reduction == DenseReductionSelection::None) {
        std::vector<RuntimeNumericElementValue> storage(
            prepared.sourceElementCount);
        for (size_t logicalIndex = 0;
             logicalIndex < prepared.sourceElementCount; ++logicalIndex) {
            RuntimeNumericElementValue value;
            if (!evaluatePreparedNodes(prepared, coordinates, registers,
                                       value, failureReason)) {
                auto result = executionFallback(
                    RuntimeFallbackKind::RuntimeFailed,
                    std::move(failureReason));
                return result;
            }
            const auto storageOffset = runtimeRowMajorStorageOffset(
                coordinates, prepared.sourceDimensions);
            if (!storageOffset || *storageOffset >= storage.size()) {
                return executionFallback(
                    RuntimeFallbackKind::RuntimeFailed,
                    "typed dense output offset is invalid");
            }
            storage[*storageOffset] = value;
            kernelInstructions += prepared.nodes.size();
            incrementColumnMajorCoordinates(
                coordinates, prepared.sourceDimensions);
        }
        const auto value = numericValueFromStorage(
            prepared.sourceDimensions, storage, prepared.numericClass);
        if (!value) {
            return executionFallback(
                RuntimeFallbackKind::RuntimeFailed,
                "typed dense result could not be represented");
        }
        output = *value;
    } else {
        const bool productReduction =
            prepared.parsed.reductionLowering ==
            BuiltinTypedLowering::Product;
        const bool meanReduction =
            prepared.parsed.reductionLowering ==
            BuiltinTypedLowering::Mean;
        std::vector<RuntimeNumericElementValue> buckets(
            prepared.outputElementCount);
        std::vector<bool> complexSeen(prepared.outputElementCount, false);
        std::vector<size_t> validCounts(prepared.outputElementCount, 0);
        for (auto& bucket : buckets) {
            bucket.numericClass = prepared.numericClass;
            bucket.real = productReduction ? 1.0 : 0.0;
        }
        for (size_t logicalIndex = 0;
             logicalIndex < prepared.sourceElementCount; ++logicalIndex) {
            RuntimeNumericElementValue value;
            if (!evaluatePreparedNodes(prepared, coordinates, registers,
                                       value, failureReason)) {
                auto result = executionFallback(
                    RuntimeFallbackKind::RuntimeFailed,
                    std::move(failureReason));
                return result;
            }
            const auto bucket = outputBucketIndex(prepared, coordinates);
            if (!bucket || *bucket >= buckets.size()) {
                return executionFallback(
                    RuntimeFallbackKind::RuntimeFailed,
                    "typed reduction output bucket is invalid");
            }
            const auto accumulated = runtimeApplyNumericElementBinary(
                productReduction ? "*" : "+", buckets[*bucket], value,
                prepared.numericClass);
            if (!accumulated) {
                return executionFallback(
                    RuntimeFallbackKind::RuntimeFailed,
                    "typed dense reduction result could not be represented");
            }
            buckets[*bucket] = *accumulated;
            complexSeen[*bucket] =
                complexSeen[*bucket] || value.complex;
            ++validCounts[*bucket];
            kernelInstructions += prepared.nodes.size() + 1;
            incrementColumnMajorCoordinates(
                coordinates, prepared.sourceDimensions);
        }
        for (size_t index = 0; index < buckets.size(); ++index) {
            if (meanReduction) {
                if (validCounts[index] == 0) {
                    buckets[index].real =
                        std::numeric_limits<double>::quiet_NaN();
                    buckets[index].imaginary = 0.0;
                } else {
                    const double count =
                        static_cast<double>(validCounts[index]);
                    buckets[index].real /= count;
                    buckets[index].imaginary /= count;
                }
            }
            if (complexSeen[index] &&
                (buckets[index].imaginary != 0.0 ||
                 std::isnan(buckets[index].imaginary))) {
                buckets[index].complex = true;
            }
        }
        const auto value = runtimeNumericValueFromElements(
            prepared.outputDimensions, std::move(buckets),
            prepared.numericClass);
        if (!value) {
            return executionFallback(
                RuntimeFallbackKind::RuntimeFailed,
                "typed reduction result could not be represented");
        }
        output = *value;
    }

    DenseArrayRegionExecutionResult result;
    result.status = TypedRegionExecutionStatus::Executed;
    result.value = std::move(output);
    result.elementCount = prepared.sourceElementCount;
    result.executedInstructionCount = region.endPc - region.beginPc;
    result.executedKernelInstructionCount = kernelInstructions;
    result.backend = TypedRegionBackend::Portable;
    result.reason = prepared.parsed.reduction ==
                            DenseReductionSelection::None
                        ? "fused portable dense element-wise kernel executed"
                        : "fused portable dense " +
                              std::string(reductionName(
                                  prepared.parsed.reductionLowering)) +
                              " kernel executed";
    return result;
}

bool nativeDomainSensitiveOperation(ScalarKernelOp operation) {
    return operation == ScalarKernelOp::ArcCosine ||
           operation == ScalarKernelOp::ArcSine ||
           operation == ScalarKernelOp::Logarithm ||
           operation == ScalarKernelOp::SquareRoot ||
           operation == ScalarKernelOp::Power;
}

bool nativeDenseDomainIsRealSafe(
    const PreparedDenseRegion& prepared, std::string& failureReason) {
    bool needsDomainCheck = false;
    for (const auto& node : prepared.nodes) {
        if (node.node.kind == DenseNodeKind::Literal &&
            node.node.literal.complex) {
            failureReason =
                "native dense kernel requires a real-valued literal domain";
            return false;
        }
        if ((node.node.kind == DenseNodeKind::Unary ||
             node.node.kind == DenseNodeKind::Binary) &&
            nativeDomainSensitiveOperation(node.node.operation)) {
            needsDomainCheck = true;
        }
    }
    if (!needsDomainCheck) {
        return true;
    }

    std::vector<RuntimeNumericElementValue> registers(
        prepared.nodes.size());
    std::vector<size_t> coordinates(
        prepared.sourceDimensions.size(), 0);
    std::string evaluationFailure;
    for (size_t index = 0; index < prepared.sourceElementCount; ++index) {
        RuntimeNumericElementValue result;
        if (!evaluatePreparedNodes(prepared, coordinates, registers, result,
                                   evaluationFailure)) {
            failureReason =
                "native dense domain preflight could not evaluate the region";
            return false;
        }
        for (const auto& value : registers) {
            if (value.complex) {
                failureReason =
                    "native dense kernel requires VM fallback for a complex-valued domain";
                return false;
            }
        }
        incrementColumnMajorCoordinates(
            coordinates, prepared.sourceDimensions);
    }
    return true;
}

RuntimeFallbackKind nativeFailureKind(NativeScalarJitStatus status) {
    switch (status) {
    case NativeScalarJitStatus::Executed:
        return RuntimeFallbackKind::None;
    case NativeScalarJitStatus::Unavailable:
        return RuntimeFallbackKind::BackendUnavailable;
    case NativeScalarJitStatus::Unsupported:
        return RuntimeFallbackKind::BackendUnsupported;
    case NativeScalarJitStatus::CompilationFailed:
        return RuntimeFallbackKind::CompilationFailed;
    case NativeScalarJitStatus::RuntimeFailed:
        return RuntimeFallbackKind::RuntimeFailed;
    }
    return RuntimeFallbackKind::RuntimeFailed;
}

struct NativeDenseKernel {
    ScalarKernel kernel;
    size_t outputArraySlot = std::numeric_limits<size_t>::max();
    size_t accumulatorSlot = std::numeric_limits<size_t>::max();
};

std::optional<NativeDenseKernel> buildNativeDenseKernel(
    const PreparedDenseRegion& prepared, std::string& failureReason) {
    for (const auto& input : prepared.inputs) {
        if (!input.scalar &&
            input.dimensions != prepared.sourceDimensions) {
            failureReason =
                "native dense kernel does not yet lower implicit expansion";
            return std::nullopt;
        }
    }

    NativeDenseKernel native;
    auto& kernel = native.kernel;
    kernel.slotNames.push_back("$dense-index");
    kernel.slots.push_back(TypedScalar{});
    kernel.initialized.push_back(true);
    kernel.loopSlot = 0;

    std::vector<size_t> scalarSlots(prepared.inputs.size(),
                                    std::numeric_limits<size_t>::max());
    std::vector<size_t> arraySlots(prepared.inputs.size(),
                                   std::numeric_limits<size_t>::max());
    for (size_t index = 0; index < prepared.inputs.size(); ++index) {
        const auto& input = prepared.inputs[index];
        if (input.scalar) {
            scalarSlots[index] = kernel.slots.size();
            kernel.slotNames.push_back(input.name);
            kernel.slots.push_back(TypedScalar{
                input.value->number, RuntimeNumericClass::Double});
            kernel.initialized.push_back(true);
        } else {
            arraySlots[index] = kernel.arrays.size();
            kernel.arraySlotNames.push_back(input.name);
            kernel.arrays.push_back(TypedNumericArray{
                input.value->kind, input.value->elements,
                input.dimensions});
        }
    }

    std::vector<ScalarKernelOperand> operands(prepared.nodes.size());
    for (size_t index = 0; index < prepared.nodes.size(); ++index) {
        const auto& node = prepared.nodes[index];
        switch (node.node.kind) {
        case DenseNodeKind::Input:
            if (prepared.inputs[node.inputIndex].scalar) {
                operands[index] = ScalarKernelOperand{
                    ScalarKernelStorage::Slot,
                    scalarSlots[node.inputIndex], {}};
            } else {
                const size_t resultRegister = kernel.registerCount++;
                ScalarKernelInstruction load;
                load.op = ScalarKernelOp::LoadArrayElement;
                load.destination = {
                    ScalarKernelStorage::Register, resultRegister};
                load.left = {ScalarKernelStorage::Slot,
                             kernel.loopSlot, {}};
                load.arraySlot = arraySlots[node.inputIndex];
                load.sourceInstructionCount = 1;
                kernel.instructions.push_back(std::move(load));
                operands[index] = ScalarKernelOperand{
                    ScalarKernelStorage::Register, resultRegister, {}};
            }
            break;
        case DenseNodeKind::Literal:
            operands[index] = ScalarKernelOperand{
                ScalarKernelStorage::Literal, 0,
                TypedScalar{node.node.literal.real,
                            RuntimeNumericClass::Double}};
            break;
        case DenseNodeKind::Unary: {
            const size_t resultRegister = kernel.registerCount++;
            ScalarKernelInstruction instruction;
            instruction.op = node.node.operation;
            instruction.destination = {
                ScalarKernelStorage::Register, resultRegister};
            instruction.left = operands[node.node.left];
            instruction.sourceInstructionCount = 1;
            kernel.instructions.push_back(std::move(instruction));
            operands[index] = ScalarKernelOperand{
                ScalarKernelStorage::Register, resultRegister, {}};
            break;
        }
        case DenseNodeKind::Binary: {
            const size_t resultRegister = kernel.registerCount++;
            ScalarKernelInstruction instruction;
            instruction.op = node.node.operation;
            instruction.destination = {
                ScalarKernelStorage::Register, resultRegister};
            instruction.left = operands[node.node.left];
            instruction.right = operands[node.node.right];
            instruction.sourceInstructionCount = 1;
            kernel.instructions.push_back(std::move(instruction));
            operands[index] = ScalarKernelOperand{
                ScalarKernelStorage::Register, resultRegister, {}};
            break;
        }
        }
    }

    const auto resultOperand = operands[prepared.parsed.resultNode];
    if (prepared.parsed.reduction == DenseReductionSelection::None) {
        native.outputArraySlot = kernel.arrays.size();
        kernel.arraySlotNames.push_back(
            prepared.parsed.analysis.target);
        kernel.arrays.push_back(TypedNumericArray{
            prepared.sourceDimensions.size() == 2 &&
                    prepared.sourceDimensions[0] == 1
                ? RuntimeValueKind::Vector
                : RuntimeValueKind::Matrix,
            std::vector<double>(prepared.sourceElementCount, 0.0),
            prepared.sourceDimensions});
        ScalarKernelInstruction store;
        store.op = ScalarKernelOp::StoreArrayElement;
        store.left = {ScalarKernelStorage::Slot, kernel.loopSlot, {}};
        store.right = resultOperand;
        store.arraySlot = native.outputArraySlot;
        store.sourceInstructionCount = 1;
        kernel.instructions.push_back(std::move(store));
    } else {
        const bool productReduction =
            prepared.parsed.reductionLowering ==
            BuiltinTypedLowering::Product;
        native.accumulatorSlot = kernel.slots.size();
        kernel.slotNames.push_back(
            "$dense-" +
            std::string(reductionName(
                prepared.parsed.reductionLowering)));
        kernel.slots.push_back(TypedScalar{
            productReduction ? 1.0 : 0.0,
            RuntimeNumericClass::Double});
        kernel.initialized.push_back(true);
        ScalarKernelInstruction accumulate;
        accumulate.op = productReduction ? ScalarKernelOp::Multiply
                                          : ScalarKernelOp::Add;
        accumulate.destination = {
            ScalarKernelStorage::Slot, native.accumulatorSlot};
        accumulate.left = {ScalarKernelStorage::Slot,
                           native.accumulatorSlot, {}};
        accumulate.right = resultOperand;
        accumulate.sourceInstructionCount = 1;
        kernel.instructions.push_back(std::move(accumulate));
    }
    return native;
}

void accumulateNativeMetadata(DenseArrayRegionExecutionResult& result,
                              const NativeScalarJitResult& native) {
    result.nativeCompilationCount += native.compiled ? 1 : 0;
    result.nativeCacheHitCount += native.cacheHit ? 1 : 0;
    result.nativeCacheInsertionCount += native.cacheStored ? 1 : 0;
    result.nativeCacheBypassCount += native.cacheBypassed ? 1 : 0;
    result.nativeCacheEvictionCount += native.cacheEvictionCount;
    result.nativeCacheEvictedCodeBytes += native.cacheEvictedCodeBytes;
    result.nativeCodeSize = std::max(result.nativeCodeSize, native.codeSize);
    result.executedKernelInstructionCount +=
        native.counters.kernelInstructions;
}

void mergeNativeAttemptMetadata(
    DenseArrayRegionExecutionResult& result,
    const DenseArrayRegionExecutionResult& native) {
    result.nativeCompilationCount += native.nativeCompilationCount;
    result.nativeCacheHitCount += native.nativeCacheHitCount;
    result.nativeCacheInsertionCount +=
        native.nativeCacheInsertionCount;
    result.nativeCacheBypassCount += native.nativeCacheBypassCount;
    result.nativeCacheEvictionCount += native.nativeCacheEvictionCount;
    result.nativeCacheEvictedCodeBytes +=
        native.nativeCacheEvictedCodeBytes;
    result.nativeCodeSize =
        std::max(result.nativeCodeSize, native.nativeCodeSize);
    result.nativePlatform = native.nativePlatform;
    result.nativeFallbackKind = native.fallbackKind;
    result.nativeFallbackReason = native.reason;
}

DenseArrayRegionExecutionResult executeNative(
    const PreparedDenseRegion& prepared,
    const BytecodeRegionContract& region) {
    DenseArrayRegionExecutionResult result;
    result.backend = TypedRegionBackend::Native;
    result.nativePlatform = std::string(nativeScalarJitPlatform());
    if (prepared.numericClass != RuntimeNumericClass::Double) {
        result.fallbackKind = RuntimeFallbackKind::BackendUnsupported;
        result.nativeFallbackKind = result.fallbackKind;
        result.reason =
            "native dense kernel currently requires double numeric inputs";
        result.nativeFallbackReason = result.reason;
        return result;
    }
    for (const auto& input : prepared.inputs) {
        if (input.value->numericComplex) {
            result.fallbackKind = RuntimeFallbackKind::BackendUnsupported;
            result.nativeFallbackKind = result.fallbackKind;
            result.reason =
                "native dense kernel currently requires real numeric inputs";
            result.nativeFallbackReason = result.reason;
            return result;
        }
    }
    if (prepared.parsed.reduction != DenseReductionSelection::None &&
        prepared.outputElementCount != 1) {
        result.fallbackKind = RuntimeFallbackKind::BackendUnsupported;
        result.nativeFallbackKind = result.fallbackKind;
        result.reason =
            "native dense reduction currently requires one output element";
        result.nativeFallbackReason = result.reason;
        return result;
    }
    std::string failureReason;
    if (!nativeDenseDomainIsRealSafe(prepared, failureReason)) {
        result.fallbackKind = RuntimeFallbackKind::BackendUnsupported;
        result.nativeFallbackKind = result.fallbackKind;
        result.reason = std::move(failureReason);
        result.nativeFallbackReason = result.reason;
        return result;
    }
    auto nativeKernel = buildNativeDenseKernel(prepared, failureReason);
    if (!nativeKernel) {
        result.fallbackKind = RuntimeFallbackKind::BackendUnsupported;
        result.nativeFallbackKind = result.fallbackKind;
        result.reason = std::move(failureReason);
        result.nativeFallbackReason = result.reason;
        return result;
    }

    if (prepared.parsed.reduction == DenseReductionSelection::None) {
        std::vector<double> storageOffsets(prepared.sourceElementCount);
        for (size_t index = 0; index < storageOffsets.size(); ++index) {
            storageOffsets[index] = static_cast<double>(index + 1);
        }
        auto native = executeNativeScalarKernel(
            nativeKernel->kernel, storageOffsets.data(),
            storageOffsets.size());
        accumulateNativeMetadata(result, native);
        if (native.status != NativeScalarJitStatus::Executed) {
            result.fallbackKind = nativeFailureKind(native.status);
            result.nativeFallbackKind = result.fallbackKind;
            result.reason = std::move(native.reason);
            result.nativeFallbackReason = result.reason;
            return result;
        }
        const auto& output =
            nativeKernel->kernel.arrays[nativeKernel->outputArraySlot];
        const auto value = numericValueFromStorage(
            output.dimensions, output.elements,
            RuntimeNumericClass::Double);
        if (!value) {
            result.fallbackKind = RuntimeFallbackKind::RuntimeFailed;
            result.nativeFallbackKind = result.fallbackKind;
            result.reason =
                "native dense result could not be represented";
            result.nativeFallbackReason = result.reason;
            return result;
        }
        result.value = *value;
    } else {
        std::vector<double> storageOffsets;
        storageOffsets.reserve(prepared.sourceElementCount);
        std::vector<size_t> coordinates(
            prepared.sourceDimensions.size(), 0);
        for (size_t logicalIndex = 0;
             logicalIndex < prepared.sourceElementCount; ++logicalIndex) {
            const auto bucket = outputBucketIndex(prepared, coordinates);
            const auto storageOffset = runtimeRowMajorStorageOffset(
                coordinates, prepared.sourceDimensions);
            if (!bucket || *bucket != 0 || !storageOffset) {
                result.fallbackKind = RuntimeFallbackKind::RuntimeFailed;
                result.nativeFallbackKind = result.fallbackKind;
                result.reason =
                    "native typed reduction could not map a source element";
                result.nativeFallbackReason = result.reason;
                return result;
            }
            storageOffsets.push_back(
                static_cast<double>(*storageOffset + 1));
            incrementColumnMajorCoordinates(
                coordinates, prepared.sourceDimensions);
        }

        const bool meanReduction =
            prepared.parsed.reductionLowering ==
            BuiltinTypedLowering::Mean;
        nativeKernel->kernel.slots[nativeKernel->accumulatorSlot] =
            TypedScalar{
                prepared.parsed.reductionLowering ==
                        BuiltinTypedLowering::Product
                    ? 1.0
                    : 0.0,
                RuntimeNumericClass::Double};
        auto native = executeNativeScalarKernel(
            nativeKernel->kernel, storageOffsets.data(),
            storageOffsets.size());
        accumulateNativeMetadata(result, native);
        if (native.status != NativeScalarJitStatus::Executed) {
            result.fallbackKind = nativeFailureKind(native.status);
            result.nativeFallbackKind = result.fallbackKind;
            result.reason = std::move(native.reason);
            result.nativeFallbackReason = result.reason;
            return result;
        }
        double accumulatedValue = nativeKernel->kernel.slots[
            nativeKernel->accumulatorSlot].value;
        if (meanReduction) {
            accumulatedValue = prepared.sourceElementCount == 0
                                   ? std::numeric_limits<double>::quiet_NaN()
                                   : accumulatedValue /
                                         static_cast<double>(
                                             prepared.sourceElementCount);
        }
        const auto value = runtimeNumericValueFromLogicalOrder(
            prepared.outputDimensions,
            {accumulatedValue},
            RuntimeNumericClass::Double);
        if (!value) {
            result.fallbackKind = RuntimeFallbackKind::RuntimeFailed;
            result.nativeFallbackKind = result.fallbackKind;
            result.reason =
                "native typed reduction result could not be represented";
            result.nativeFallbackReason = result.reason;
            return result;
        }
        result.value = *value;
    }

    result.status = TypedRegionExecutionStatus::Executed;
    result.elementCount = prepared.sourceElementCount;
    result.executedInstructionCount = region.endPc - region.beginPc;
    result.reason = prepared.parsed.reduction ==
                            DenseReductionSelection::None
                        ? "fused native dense element-wise kernel executed"
                        : "fused native dense " +
                              std::string(reductionName(
                                  prepared.parsed.reductionLowering)) +
                              " kernel executed";
    return result;
}

} // namespace

DenseArrayRegionAnalysis analyzeDenseArrayAssignmentRegion(
    const BytecodeProgram& program, size_t storePc,
    const BuiltinRegistry& builtinRegistry) {
    return parseDenseRegion(program, storePc, builtinRegistry).analysis;
}

DenseArrayTypedRegionExecutor::DenseArrayTypedRegionExecutor()
    : builtinRegistry_(defaultBuiltinRegistry()) {}

DenseArrayTypedRegionExecutor::DenseArrayTypedRegionExecutor(
    std::shared_ptr<const BuiltinRegistry> builtinRegistry)
    : builtinRegistry_(builtinRegistry
                           ? std::move(builtinRegistry)
                           : defaultBuiltinRegistry()) {}

DenseArrayRegionExecutionResult DenseArrayTypedRegionExecutor::execute(
    const BytecodeProgram& program,
    const BytecodeRegionContract& region,
    const RuntimeWorkspace& variables, TypedRegionBackend backend) const {
    if (region.outputs.size() != 1) {
        return executionFallback(
            RuntimeFallbackKind::InvalidContract,
            "dense typed region must have exactly one output");
    }
    BytecodeRegionAnalyzer analyzer(builtinRegistry_);
    const auto expected = analyzer.analyze(
        program, "dense-array-assignment", region.bodyEndPc,
        region.outputs.front());
    if (!bytecodeRegionContractsEquivalent(region, expected)) {
        return executionFallback(
            RuntimeFallbackKind::InvalidContract,
            "dense typed region contract does not match bytecode");
    }
    return executeValidated(program, region, variables, backend);
}

DenseArrayRegionExecutionResult
DenseArrayTypedRegionExecutor::executeValidated(
    const BytecodeProgram& program,
    const BytecodeRegionContract& region,
    const RuntimeWorkspace& variables, TypedRegionBackend backend) const {
    if (!region.eligibleForTypedExecution ||
        region.reductionOperationCount > 1) {
        return executionFallback(
            region.fallbackKind == RuntimeFallbackKind::None
                ? RuntimeFallbackKind::InvalidContract
                : region.fallbackKind,
            region.reason);
    }
    std::string failureReason;
    const auto prepared = prepareDenseRegion(
        program, region, variables, *builtinRegistry_, failureReason);
    if (!prepared) {
        return executionFallback(
            RuntimeFallbackKind::UnsupportedInput,
            std::move(failureReason));
    }

    std::optional<DenseArrayRegionExecutionResult> nativeAttempt;
    if (backend != TypedRegionBackend::Portable) {
        auto native = executeNative(*prepared, region);
        if (native.status == TypedRegionExecutionStatus::Executed) {
            return native;
        }
        if (backend == TypedRegionBackend::Native) {
            return native;
        }
        nativeAttempt = std::move(native);
    }

    auto portable = executePortable(*prepared, region);
    if (nativeAttempt) {
        mergeNativeAttemptMetadata(portable, *nativeAttempt);
    }
    return portable;
}

} // namespace mparser
