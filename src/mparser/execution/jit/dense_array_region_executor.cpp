#include "mparser/execution/jit/dense_array_region_executor.h"

#include "mparser/execution/jit/native_scalar_jit.h"
#include "mparser/execution/jit/typed_scalar_kernel.h"
#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/builtins/numeric/runtime_reduction.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"

#include <algorithm>
#include <cmath>
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
    double literal = 0.0;
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
};

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

std::optional<double> doubleLiteral(std::string_view text) {
    const auto value = runtimeParseNumericLiteral(text);
    if (!value || value->kind != RuntimeValueKind::Number ||
        value->numericClass != RuntimeNumericClass::Double ||
        value->numericComplex) {
        return std::nullopt;
    }
    return value->number;
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
            const auto value = doubleLiteral(instruction.operand);
            if (!value) {
                return reject(
                    "dense expression contains a nondouble literal");
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
                parsed.reduction = DenseReductionSelection::Default;
                if (argumentCount == 2) {
                    const auto& option = stack[callableIndex + 2];
                    if (option.kind == ParseStackKind::AllOption) {
                        parsed.reduction = DenseReductionSelection::All;
                    } else if (option.kind == ParseStackKind::Node) {
                        const auto& optionNode = parsed.nodes[option.node];
                        const auto dimension =
                            optionNode.kind == DenseNodeKind::Literal
                                ? checkedRuntimeNonnegativeInteger(
                                      optionNode.literal)
                                : std::nullopt;
                        if (!dimension || *dimension == 0) {
                            return reject(
                                "typed sum dimension must be a positive integer literal",
                                true);
                        }
                        parsed.reduction =
                            DenseReductionSelection::Dimension;
                        parsed.reductionDimension = *dimension - 1;
                    } else {
                        return reject(
                            "typed sum option must be a dimension or \"all\"",
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
                          : "eligible closed fused dense sum assignment";
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
        if (value.kind == RuntimeValueKind::Number &&
            value.numericClass == RuntimeNumericClass::Double &&
            !value.numericComplex) {
            input.scalar = true;
        } else if ((value.kind == RuntimeValueKind::Vector ||
                    value.kind == RuntimeValueKind::Matrix) &&
                   value.numericClass == RuntimeNumericClass::Double &&
                   !value.numericComplex) {
            const auto count =
                checkedRuntimeDimensionProduct(input.dimensions);
            if (!count || *count != value.elements.size()) {
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
                "typed dense input is not a real double scalar or dense array: " +
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

bool evaluateUnary(ScalarKernelOp operation, double input,
                   double& result, std::string& failureReason) {
    const bool inverseDomain =
        (operation == ScalarKernelOp::ArcCosine ||
         operation == ScalarKernelOp::ArcSine) &&
        std::fabs(input) > 1.0;
    const bool negativeDomain =
        (operation == ScalarKernelOp::Logarithm ||
         operation == ScalarKernelOp::SquareRoot) &&
        input < 0.0;
    if (inverseDomain || negativeDomain) {
        failureReason =
            "typed dense math operation requires a complex result";
        return false;
    }
    switch (operation) {
    case ScalarKernelOp::Copy:
    case ScalarKernelOp::UnaryPlus:
        result = input;
        return true;
    case ScalarKernelOp::UnaryMinus:
        result = -input;
        return true;
    case ScalarKernelOp::Absolute:
        result = std::fabs(input);
        return true;
    case ScalarKernelOp::ArcCosine:
        result = std::acos(input);
        return true;
    case ScalarKernelOp::ArcSine:
        result = std::asin(input);
        return true;
    case ScalarKernelOp::ArcTangent:
        result = std::atan(input);
        return true;
    case ScalarKernelOp::Cosine:
        result = std::cos(input);
        return true;
    case ScalarKernelOp::Exponential:
        result = std::exp(input);
        return true;
    case ScalarKernelOp::Logarithm:
        result = std::log(input);
        return true;
    case ScalarKernelOp::Sine:
        result = std::sin(input);
        return true;
    case ScalarKernelOp::SquareRoot:
        result = std::sqrt(input);
        return true;
    case ScalarKernelOp::Tangent:
        result = std::tan(input);
        return true;
    default:
        failureReason = "typed dense unary operation is unsupported";
        return false;
    }
}

bool evaluateBinary(ScalarKernelOp operation, double left, double right,
                    double& result, std::string& failureReason) {
    if (operation == ScalarKernelOp::Power && left < 0.0 &&
        std::isfinite(right) && std::floor(right) != right) {
        failureReason =
            "typed dense power requires a complex result";
        return false;
    }
    switch (operation) {
    case ScalarKernelOp::Add:
        result = left + right;
        return true;
    case ScalarKernelOp::Subtract:
        result = left - right;
        return true;
    case ScalarKernelOp::Multiply:
        result = left * right;
        return true;
    case ScalarKernelOp::Divide:
        result = left / right;
        return true;
    case ScalarKernelOp::Power:
        result = std::pow(left, right);
        return true;
    default:
        failureReason = "typed dense binary operation is unsupported";
        return false;
    }
}

bool evaluatePreparedNodes(
    const PreparedDenseRegion& prepared,
    const std::vector<size_t>& coordinates,
    std::vector<double>& registers, double& result,
    std::string& failureReason) {
    for (size_t index = 0; index < prepared.nodes.size(); ++index) {
        const auto& node = prepared.nodes[index];
        switch (node.node.kind) {
        case DenseNodeKind::Input: {
            const auto& input = prepared.inputs[node.inputIndex];
            if (input.scalar) {
                registers[index] = input.value->number;
                break;
            }
            const auto offset = inputStorageOffset(input, coordinates);
            if (!offset || *offset >= input.value->elements.size()) {
                failureReason =
                    "typed dense input offset is outside its guarded shape";
                return false;
            }
            registers[index] = input.value->elements[*offset];
            break;
        }
        case DenseNodeKind::Literal:
            registers[index] = node.node.literal;
            break;
        case DenseNodeKind::Unary:
            if (!evaluateUnary(node.node.operation,
                               registers[node.node.left],
                               registers[index], failureReason)) {
                return false;
            }
            break;
        case DenseNodeKind::Binary:
            if (!evaluateBinary(node.node.operation,
                                registers[node.node.left],
                                registers[node.node.right],
                                registers[index], failureReason)) {
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

RuntimeValue doubleValueFromStorage(
    std::vector<size_t> dimensions, std::vector<double> storage) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    RuntimeValue result;
    result.numericClass = RuntimeNumericClass::Double;
    if (storage.size() == 1) {
        result.kind = RuntimeValueKind::Number;
        result.number = storage.front();
    } else {
        result.kind = dimensions.size() == 2 && dimensions[0] == 1
                          ? RuntimeValueKind::Vector
                          : RuntimeValueKind::Matrix;
        result.elements = std::move(storage);
    }
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

DenseArrayRegionExecutionResult executePortable(
    const PreparedDenseRegion& prepared,
    const BytecodeRegionContract& region) {
    std::vector<double> registers(prepared.nodes.size(), 0.0);
    std::vector<size_t> coordinates(
        prepared.sourceDimensions.size(), 0);
    std::string failureReason;
    RuntimeValue output;
    size_t kernelInstructions = 0;

    if (prepared.parsed.reduction == DenseReductionSelection::None) {
        std::vector<double> storage(prepared.sourceElementCount, 0.0);
        for (size_t logicalIndex = 0;
             logicalIndex < prepared.sourceElementCount; ++logicalIndex) {
            double value = 0.0;
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
        output = doubleValueFromStorage(
            prepared.sourceDimensions, std::move(storage));
    } else {
        std::vector<double> buckets(prepared.outputElementCount, 0.0);
        for (size_t logicalIndex = 0;
             logicalIndex < prepared.sourceElementCount; ++logicalIndex) {
            double value = 0.0;
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
                    "typed sum output bucket is invalid");
            }
            buckets[*bucket] += value;
            kernelInstructions += prepared.nodes.size() + 1;
            incrementColumnMajorCoordinates(
                coordinates, prepared.sourceDimensions);
        }
        const auto value = runtimeNumericValueFromLogicalOrder(
            prepared.outputDimensions, std::move(buckets),
            RuntimeNumericClass::Double);
        if (!value) {
            return executionFallback(
                RuntimeFallbackKind::RuntimeFailed,
                "typed sum result could not be represented");
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
                        : "fused portable dense sum kernel executed";
    return result;
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
            kernel.slots.push_back(TypedScalar{input.value->number});
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
                TypedScalar{node.node.literal}};
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
        native.accumulatorSlot = kernel.slots.size();
        kernel.slotNames.push_back("$dense-sum");
        kernel.slots.push_back(TypedScalar{});
        kernel.initialized.push_back(true);
        ScalarKernelInstruction accumulate;
        accumulate.op = ScalarKernelOp::Add;
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
        result.value = doubleValueFromStorage(
            output.dimensions, output.elements);
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
                    "native typed sum could not map a source element";
                result.nativeFallbackReason = result.reason;
                return result;
            }
            storageOffsets.push_back(
                static_cast<double>(*storageOffset + 1));
            incrementColumnMajorCoordinates(
                coordinates, prepared.sourceDimensions);
        }

        nativeKernel->kernel.slots[
            nativeKernel->accumulatorSlot] = TypedScalar{};
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
        const auto value = runtimeNumericValueFromLogicalOrder(
            prepared.outputDimensions,
            {nativeKernel->kernel.slots[
                 nativeKernel->accumulatorSlot].value},
            RuntimeNumericClass::Double);
        if (!value) {
            result.fallbackKind = RuntimeFallbackKind::RuntimeFailed;
            result.nativeFallbackKind = result.fallbackKind;
            result.reason =
                "native typed sum result could not be represented";
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
                        : "fused native dense sum kernel executed";
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
