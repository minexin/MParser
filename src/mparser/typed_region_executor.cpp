#include "mparser/typed_region_executor.h"
#include "mparser/builtin_registry.h"
#include "mparser/native_scalar_jit.h"
#include "mparser/runtime_range.h"
#include "mparser/runtime_shape.h"
#include "mparser/typed_scalar_kernel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser {

std::string_view typedRegionBackendName(TypedRegionBackend backend) {
    switch (backend) {
    case TypedRegionBackend::Auto:
        return "auto";
    case TypedRegionBackend::Portable:
        return "portable";
    case TypedRegionBackend::Native:
        return "native";
    }
    return "unknown";
}

namespace {

struct ScalarKernelRange {
    ScalarKernelOperand start;
    ScalarKernelOperand step;
    ScalarKernelOperand stop;
    bool singleValue = false;
};

struct ScalarKernelStackValue {
    bool isRange = false;
    bool isArray = false;
    size_t arraySlot = std::numeric_limits<size_t>::max();
    ScalarKernelOperand scalar;
    ScalarKernelRange range;
};

struct ScalarKernelCompileLoop {
    size_t beginInstruction = 0;
    size_t expectedLatchPc = 0;
    size_t loopSlot = 0;
    std::string variable;
    std::vector<bool> initializedBefore;
};

struct ScalarKernelBranchPatch {
    size_t instruction = 0;
    size_t sourcePc = 0;
    size_t targetPc = 0;
};

struct LoopRangeView {
    const std::vector<double>* values = nullptr;
    double scalar = 0.0;
    bool isScalar = false;

    size_t size() const {
        return isScalar ? 1 : values->size();
    }

    double operator[](size_t index) const {
        return isScalar ? scalar : (*values)[index];
    }
};

RuntimeValue numberValue(const TypedScalar& value) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Number;
    result.number = value.value;
    result.numericClass = value.numericClass;
    setRuntimeDimensions(result, {1, 1});
    return result;
}

RuntimeValue arrayValue(const TypedNumericArray& value) {
    RuntimeValue result;
    result.kind = value.kind;
    result.elements = value.elements;
    result.numericClass = RuntimeNumericClass::Double;
    setRuntimeDimensions(result, value.dimensions);
    return result;
}

bool isDirectLinearDoubleArray(const RuntimeValue& value) {
    if ((value.kind != RuntimeValueKind::Vector &&
         value.kind != RuntimeValueKind::Matrix) ||
        value.numericClass != RuntimeNumericClass::Double ||
        value.numericComplex) {
        return false;
    }
    const auto dimensions = runtimeDimensions(value);
    const auto elementCount =
        checkedRuntimeDimensionProduct(dimensions);
    if (!elementCount || *elementCount != value.elements.size()) {
        return false;
    }
    return std::count_if(
               dimensions.begin(), dimensions.end(),
               [](size_t dimension) { return dimension != 1; }) <= 1;
}

bool isScalarStackValue(const ScalarKernelStackValue& value) {
    return !value.isRange && !value.isArray;
}

std::optional<double> parseNumber(std::string_view text) {
    const std::string buffer(text);
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end == buffer.c_str() || end != buffer.c_str() + buffer.size()) {
        return std::nullopt;
    }
    return value;
}

bool truthy(double value) {
    return value != 0.0 && !std::isnan(value);
}

std::optional<ScalarKernelOp> unaryOperation(std::string_view operation) {
    if (operation == "+") {
        return ScalarKernelOp::UnaryPlus;
    }
    if (operation == "-") {
        return ScalarKernelOp::UnaryMinus;
    }
    if (operation == "~") {
        return ScalarKernelOp::LogicalNot;
    }
    return std::nullopt;
}

std::optional<ScalarKernelOp> binaryOperation(std::string_view operation) {
    if (operation == "+") {
        return ScalarKernelOp::Add;
    }
    if (operation == "-") {
        return ScalarKernelOp::Subtract;
    }
    if (operation == "*" || operation == ".*") {
        return ScalarKernelOp::Multiply;
    }
    if (operation == "/" || operation == "./") {
        return ScalarKernelOp::Divide;
    }
    if (operation == "^" || operation == ".^") {
        return ScalarKernelOp::Power;
    }
    if (operation == ">") {
        return ScalarKernelOp::Greater;
    }
    if (operation == "<") {
        return ScalarKernelOp::Less;
    }
    if (operation == ">=") {
        return ScalarKernelOp::GreaterEqual;
    }
    if (operation == "<=") {
        return ScalarKernelOp::LessEqual;
    }
    if (operation == "==") {
        return ScalarKernelOp::Equal;
    }
    if (operation == "~=") {
        return ScalarKernelOp::NotEqual;
    }
    if (operation == "&" || operation == "&&") {
        return ScalarKernelOp::LogicalAnd;
    }
    if (operation == "|" || operation == "||") {
        return ScalarKernelOp::LogicalOr;
    }
    return std::nullopt;
}

std::optional<ScalarKernelOp> mathOperation(
    BuiltinTypedLowering lowering) {
    if (lowering == BuiltinTypedLowering::Absolute) {
        return ScalarKernelOp::Absolute;
    }
    if (lowering == BuiltinTypedLowering::ArcCosine) {
        return ScalarKernelOp::ArcCosine;
    }
    if (lowering == BuiltinTypedLowering::ArcSine) {
        return ScalarKernelOp::ArcSine;
    }
    if (lowering == BuiltinTypedLowering::ArcTangent) {
        return ScalarKernelOp::ArcTangent;
    }
    if (lowering == BuiltinTypedLowering::Cosine) {
        return ScalarKernelOp::Cosine;
    }
    if (lowering == BuiltinTypedLowering::Exponential) {
        return ScalarKernelOp::Exponential;
    }
    if (lowering == BuiltinTypedLowering::Logarithm) {
        return ScalarKernelOp::Logarithm;
    }
    if (lowering == BuiltinTypedLowering::Sine) {
        return ScalarKernelOp::Sine;
    }
    if (lowering == BuiltinTypedLowering::SquareRoot) {
        return ScalarKernelOp::SquareRoot;
    }
    if (lowering == BuiltinTypedLowering::Tangent) {
        return ScalarKernelOp::Tangent;
    }
    return std::nullopt;
}

std::optional<ScalarKernelOp> mathOperation(
    const BuiltinRegistry& builtinRegistry,
    std::string_view name) {
    const BuiltinDescriptor* descriptor =
        builtinRegistry.find(name);
    return descriptor ? mathOperation(descriptor->typedLowering)
                      : std::nullopt;
}

std::optional<LoopRangeView> loopValues(const RuntimeValue& range) {
    if (range.numericClass != RuntimeNumericClass::Double ||
        range.numericComplex) {
        return std::nullopt;
    }
    if (range.kind == RuntimeValueKind::Number) {
        return LoopRangeView{nullptr, range.number, true};
    }
    if (range.kind == RuntimeValueKind::Vector ||
        range.kind == RuntimeValueKind::Matrix) {
        return LoopRangeView{&range.elements, 0.0, false};
    }
    return std::nullopt;
}

TypedRegionExecutionResult fallback(RuntimeFallbackKind kind,
                                    std::string reason) {
    TypedRegionExecutionResult result;
    result.fallbackKind = kind;
    result.reason = std::move(reason);
    return result;
}

RuntimeFallbackKind nativeFallbackKind(
    NativeScalarJitStatus status) {
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

size_t findOrAddSlot(ScalarKernel& kernel, std::string_view name,
                     const RuntimeWorkspace& variables) {
    const auto existing = std::find(kernel.slotNames.begin(),
                                    kernel.slotNames.end(), name);
    if (existing != kernel.slotNames.end()) {
        return static_cast<size_t>(
            std::distance(kernel.slotNames.begin(), existing));
    }

    const size_t slot = kernel.slotNames.size();
    kernel.slotNames.emplace_back(name);
    kernel.slots.emplace_back();
    kernel.initialized.push_back(false);

    const auto variable = variables.find(kernel.slotNames.back());
    if (variable != variables.end() &&
        variable->second.kind == RuntimeValueKind::Number) {
        kernel.slots.back() = TypedScalar{variable->second.number,
                                          variable->second.numericClass};
        kernel.initialized.back() = true;
    }
    return slot;
}

std::optional<size_t> findSlot(const ScalarKernel& kernel,
                               std::string_view name) {
    const auto found =
        std::find(kernel.slotNames.begin(), kernel.slotNames.end(), name);
    if (found == kernel.slotNames.end()) {
        return std::nullopt;
    }
    return static_cast<size_t>(
        std::distance(kernel.slotNames.begin(), found));
}

std::optional<size_t> findArraySlot(const ScalarKernel& kernel,
                                    std::string_view name) {
    const auto found = std::find(kernel.arraySlotNames.begin(),
                                 kernel.arraySlotNames.end(), name);
    if (found == kernel.arraySlotNames.end()) {
        return std::nullopt;
    }
    return static_cast<size_t>(
        std::distance(kernel.arraySlotNames.begin(), found));
}

size_t findOrAddArraySlot(
    ScalarKernel& kernel, std::string_view name,
    const RuntimeValue& value) {
    if (const auto existing = findArraySlot(kernel, name)) {
        return *existing;
    }
    const size_t slot = kernel.arraySlotNames.size();
    kernel.arraySlotNames.emplace_back(name);
    kernel.arrays.push_back(TypedNumericArray{
        value.kind, value.elements, runtimeDimensions(value)});
    return slot;
}

bool requireStack(const std::vector<ScalarKernelStackValue>& stack,
                  size_t required, std::string reason,
                  std::string& failureReason) {
    if (stack.size() >= required) {
        return true;
    }
    failureReason = std::move(reason);
    return false;
}

std::optional<ScalarKernel> compileKernel(
    const BytecodeProgram& program, const BytecodeRegionContract& region,
    const RuntimeWorkspace& variables,
    const BuiltinRegistry& builtinRegistry,
    std::string& failureReason) {
    ScalarKernel kernel;
    kernel.instructions.reserve(region.bodyEndPc - region.bodyBeginPc);

    const auto& header = program.instructions[region.beginPc];
    kernel.loopSlot = findOrAddSlot(kernel, header.operand, variables);
    kernel.initialized[kernel.loopSlot] = true;
    kernel.slots[kernel.loopSlot] = TypedScalar{};
    for (const auto& input : region.inputs) {
        const auto variable = variables.find(input);
        if (variable == variables.end()) {
            failureReason = "typed region input is unavailable: " + input;
            return std::nullopt;
        }
        if (variable->second.kind == RuntimeValueKind::Number &&
            variable->second.numericClass == RuntimeNumericClass::Double &&
            !variable->second.numericComplex) {
            const size_t slot = findOrAddSlot(kernel, input, variables);
            if (!kernel.initialized[slot]) {
                failureReason =
                    "typed region input is unavailable: " + input;
                return std::nullopt;
            }
            continue;
        }
        if (isDirectLinearDoubleArray(variable->second)) {
            findOrAddArraySlot(kernel, input, variable->second);
            continue;
        }
        failureReason =
            "typed region input is not a supported double scalar or linear array: " +
            input;
        return std::nullopt;
    }

    std::vector<ScalarKernelStackValue> stack;
    stack.reserve(region.bodyEndPc - region.bodyBeginPc);
    std::vector<ScalarKernelCompileLoop> loops;
    std::vector<ScalarKernelBranchPatch> branchPatches;
    std::vector<size_t> pcToKernel(
        region.bodyEndPc - region.bodyBeginPc + 1,
        std::numeric_limits<size_t>::max());
    size_t pendingSourceInstructionCount = 0;

    const auto appendInstruction = [&](ScalarKernelInstruction value) {
        value.sourceInstructionCount = pendingSourceInstructionCount;
        pendingSourceInstructionCount = 0;
        kernel.instructions.push_back(std::move(value));
    };
    const auto appendUnary = [&](ScalarKernelOp operation,
                                 ScalarKernelOperand operand) {
        const size_t result = kernel.registerCount++;
        ScalarKernelInstruction value;
        value.op = operation;
        value.destination = {ScalarKernelStorage::Register, result};
        value.left = operand;
        appendInstruction(std::move(value));
        return ScalarKernelOperand{ScalarKernelStorage::Register, result,
                                   {}};
    };
    const auto appendBinary = [&](ScalarKernelOp operation,
                                  ScalarKernelOperand left,
                                  ScalarKernelOperand right) {
        const size_t result = kernel.registerCount++;
        ScalarKernelInstruction value;
        value.op = operation;
        value.destination = {ScalarKernelStorage::Register, result};
        value.left = left;
        value.right = right;
        appendInstruction(std::move(value));
        return ScalarKernelOperand{ScalarKernelStorage::Register, result,
                                   {}};
    };

    for (size_t pc = region.bodyBeginPc; pc < region.bodyEndPc; ++pc) {
        pcToKernel[pc - region.bodyBeginPc] =
            kernel.instructions.size();
        const auto& instruction = program.instructions[pc];
        ++pendingSourceInstructionCount;
        switch (instruction.op) {
        case BytecodeOp::LoadName: {
            const BuiltinDescriptor* descriptor =
                builtinRegistry.find(instruction.operand);
            if (instruction.binding.kind == BindingKind::Builtin &&
                descriptor &&
                descriptor->typedLowering !=
                    BuiltinTypedLowering::None) {
                break;
            }
            if (const auto slot = findSlot(kernel, instruction.operand)) {
                if (!kernel.initialized[*slot]) {
                    failureReason =
                        "typed region load is unavailable: " +
                        instruction.operand;
                    return std::nullopt;
                }
                ScalarKernelStackValue value;
                value.scalar = {ScalarKernelStorage::Slot, *slot, {}};
                stack.push_back(value);
                break;
            }
            if (const auto arraySlot =
                    findArraySlot(kernel, instruction.operand)) {
                ScalarKernelStackValue value;
                value.isArray = true;
                value.arraySlot = *arraySlot;
                stack.push_back(value);
                break;
            }
            const auto variable = variables.find(instruction.operand);
            if (variable == variables.end()) {
                failureReason = "typed region load is unavailable: " +
                                instruction.operand;
                return std::nullopt;
            }
            if (variable->second.kind == RuntimeValueKind::Number) {
                const size_t slot = findOrAddSlot(
                    kernel, instruction.operand, variables);
                if (!kernel.initialized[slot]) {
                    failureReason =
                        "typed region load is unavailable: " +
                        instruction.operand;
                    return std::nullopt;
                }
                ScalarKernelStackValue value;
                value.scalar = {ScalarKernelStorage::Slot, slot, {}};
                stack.push_back(value);
                break;
            }
            if (isDirectLinearDoubleArray(variable->second)) {
                ScalarKernelStackValue value;
                value.isArray = true;
                value.arraySlot = findOrAddArraySlot(
                    kernel, instruction.operand, variable->second);
                stack.push_back(value);
                break;
            }
            failureReason =
                "typed region load is not a supported scalar or linear array: " +
                instruction.operand;
            return std::nullopt;
        }
        case BytecodeOp::LoadLiteral: {
            const auto value = parseNumber(instruction.operand);
            if (!value) {
                failureReason = "typed region literal is not numeric";
                return std::nullopt;
            }
            ScalarKernelStackValue stackValue;
            stackValue.scalar = {ScalarKernelStorage::Literal, 0,
                                 TypedScalar{*value}};
            stack.push_back(stackValue);
            break;
        }
        case BytecodeOp::StoreName: {
            if (!requireStack(stack, 1,
                              "typed region stack underflow at store",
                              failureReason)) {
                return std::nullopt;
            }
            if (!isScalarStackValue(stack.back())) {
                failureReason =
                    "typed region cannot store a range or array as a scalar";
                return std::nullopt;
            }
            if (findArraySlot(kernel, instruction.operand)) {
                failureReason =
                    "typed region cannot change an array binding to a scalar";
                return std::nullopt;
            }
            const size_t slot = findOrAddSlot(
                kernel, instruction.operand, variables);
            const auto source = stack.back().scalar;
            stack.pop_back();
            if (source.storage == ScalarKernelStorage::Register &&
                !kernel.instructions.empty() &&
                kernel.instructions.back().destination.storage ==
                    ScalarKernelStorage::Register &&
                kernel.instructions.back().destination.index ==
                    source.index) {
                kernel.instructions.back().destination =
                    {ScalarKernelStorage::Slot, slot};
                kernel.instructions.back().sourceInstructionCount +=
                    pendingSourceInstructionCount;
                pendingSourceInstructionCount = 0;
            } else {
                ScalarKernelInstruction value;
                value.op = ScalarKernelOp::Copy;
                value.destination = {ScalarKernelStorage::Slot, slot};
                value.left = source;
                appendInstruction(std::move(value));
            }
            kernel.initialized[slot] = true;
            break;
        }
        case BytecodeOp::UnaryOp: {
            if (!requireStack(
                    stack, 1,
                    "typed region stack underflow at unary operation",
                    failureReason)) {
                return std::nullopt;
            }
            if (!isScalarStackValue(stack.back())) {
                failureReason =
                    "typed region unary operation received a non-scalar";
                return std::nullopt;
            }
            const auto operation = unaryOperation(instruction.operand);
            if (!operation) {
                failureReason =
                    "typed region unary operation is unsupported";
                return std::nullopt;
            }
            stack.back().scalar =
                appendUnary(*operation, stack.back().scalar);
            break;
        }
        case BytecodeOp::BinaryOp: {
            const size_t operandCount =
                instruction.operandCount < 0
                    ? 0
                    : static_cast<size_t>(instruction.operandCount);
            if (instruction.operand == ":") {
                if ((operandCount != 2 && operandCount != 3) ||
                    !requireStack(
                        stack, operandCount,
                        "typed region stack underflow at colon operation",
                        failureReason)) {
                    if (failureReason.empty()) {
                        failureReason =
                            "typed colon range needs two or three operands";
                    }
                    return std::nullopt;
                }
                const size_t begin = stack.size() - operandCount;
                for (size_t index = begin; index < stack.size(); ++index) {
                    if (!isScalarStackValue(stack[index])) {
                        failureReason =
                            "typed colon range operand is not scalar";
                        return std::nullopt;
                    }
                }
                ScalarKernelStackValue value;
                value.isRange = true;
                value.range.start = stack[begin].scalar;
                value.range.step =
                    operandCount == 3
                        ? stack[begin + 1].scalar
                        : ScalarKernelOperand{
                              ScalarKernelStorage::Literal, 0,
                              TypedScalar{1.0}};
                value.range.stop = stack.back().scalar;
                stack.resize(begin);
                stack.push_back(value);
                break;
            }

            if (!requireStack(
                    stack, 2,
                    "typed region stack underflow at binary operation",
                    failureReason)) {
                return std::nullopt;
            }
            if (!isScalarStackValue(stack[stack.size() - 2]) ||
                !isScalarStackValue(stack.back())) {
                failureReason =
                    "typed region binary operation received a non-scalar";
                return std::nullopt;
            }
            const auto operation = binaryOperation(instruction.operand);
            if (!operation) {
                failureReason =
                    "typed region binary operation is unsupported";
                return std::nullopt;
            }
            const auto right = stack.back().scalar;
            stack.pop_back();
            const auto left = stack.back().scalar;
            stack.back().scalar = appendBinary(*operation, left, right);
            break;
        }
        case BytecodeOp::CallOrIndex: {
            if (instruction.binding.kind == BindingKind::Builtin) {
                if (instruction.operandCount != 1 ||
                    instruction.resultCount != 1) {
                    failureReason =
                        "typed region builtin call has an invalid arity";
                    return std::nullopt;
                }
                const auto operation =
                    mathOperation(builtinRegistry,
                                  instruction.calleeName);
                if (!operation) {
                    failureReason =
                        "typed region builtin call is unsupported";
                    return std::nullopt;
                }
                if (!requireStack(
                        stack, 1,
                        "typed region stack underflow at builtin call",
                        failureReason)) {
                    return std::nullopt;
                }
                if (!isScalarStackValue(stack.back())) {
                    failureReason =
                        "typed region builtin received a non-scalar";
                    return std::nullopt;
                }
                stack.back().scalar =
                    appendUnary(*operation, stack.back().scalar);
                break;
            }

            if (instruction.operandCount != 1 ||
                instruction.resultCount != 1 ||
                !instruction.calleeName.empty() ||
                (!instruction.colonSubscripts.empty() &&
                 instruction.colonSubscripts.front())) {
                failureReason =
                    "typed region index is not a direct linear read";
                return std::nullopt;
            }
            if (!requireStack(
                    stack, 2,
                    "typed region stack underflow at linear index read",
                    failureReason)) {
                return std::nullopt;
            }
            const size_t arrayPosition = stack.size() - 2;
            if (!stack[arrayPosition].isArray ||
                !isScalarStackValue(stack.back())) {
                failureReason =
                    "typed region linear index read needs an array and scalar index";
                return std::nullopt;
            }
            const size_t resultRegister = kernel.registerCount++;
            ScalarKernelInstruction value;
            value.op = ScalarKernelOp::LoadArrayElement;
            value.destination = {ScalarKernelStorage::Register,
                                 resultRegister};
            value.left = stack.back().scalar;
            value.arraySlot = stack[arrayPosition].arraySlot;
            appendInstruction(std::move(value));
            stack.resize(arrayPosition);
            ScalarKernelStackValue resultValue;
            resultValue.scalar = {ScalarKernelStorage::Register,
                                  resultRegister, {}};
            stack.push_back(resultValue);
            break;
        }
        case BytecodeOp::StoreIndex: {
            if (instruction.operandCount != 1 ||
                instruction.nullAssignment ||
                instruction.nondeterministicAssignment ||
                (!instruction.colonSubscripts.empty() &&
                 instruction.colonSubscripts.front())) {
                failureReason =
                    "typed region index is not a direct preallocated linear write";
                return std::nullopt;
            }
            if (!requireStack(
                    stack, 3,
                    "typed region stack underflow at linear index write",
                    failureReason)) {
                return std::nullopt;
            }
            const size_t valuePosition = stack.size() - 3;
            const size_t arrayPosition = stack.size() - 2;
            const size_t indexPosition = stack.size() - 1;
            if (!isScalarStackValue(stack[valuePosition]) ||
                !stack[arrayPosition].isArray ||
                !isScalarStackValue(stack[indexPosition])) {
                failureReason =
                    "typed region linear index write needs a scalar value, array, and scalar index";
                return std::nullopt;
            }
            const size_t arraySlot = stack[arrayPosition].arraySlot;
            if (arraySlot >= kernel.arraySlotNames.size() ||
                kernel.arraySlotNames[arraySlot] != instruction.operand) {
                failureReason =
                    "typed region linear index target does not match its array binding";
                return std::nullopt;
            }
            ScalarKernelInstruction value;
            value.op = ScalarKernelOp::StoreArrayElement;
            value.left = stack[indexPosition].scalar;
            value.right = stack[valuePosition].scalar;
            value.arraySlot = arraySlot;
            appendInstruction(std::move(value));
            stack.resize(valuePosition);
            break;
        }
        case BytecodeOp::BeginIndexContext:
        case BytecodeOp::BeginIndexArgument:
            break;
        case BytecodeOp::PostfixOp:
            if (!requireStack(
                    stack, 1,
                    "typed region stack underflow at postfix operation",
                    failureReason)) {
                return std::nullopt;
            }
            if (instruction.operand != "'") {
                failureReason =
                    "typed region postfix operation is unsupported";
                return std::nullopt;
            }
            if (!isScalarStackValue(stack.back())) {
                failureReason =
                    "typed region transpose received a non-scalar";
                return std::nullopt;
            }
            break;
        case BytecodeOp::Pop:
            if (!requireStack(stack, 1,
                              "typed region stack underflow at pop",
                              failureReason)) {
                return std::nullopt;
            }
            if (!isScalarStackValue(stack.back())) {
                failureReason =
                    "typed region cannot discard a non-scalar";
                return std::nullopt;
            }
            {
                ScalarKernelInstruction value;
                value.op = ScalarKernelOp::Discard;
                value.left = stack.back().scalar;
                appendInstruction(std::move(value));
            }
            stack.pop_back();
            break;
        case BytecodeOp::Jump:
            if (!stack.empty() || instruction.target < 0) {
                failureReason =
                    "typed branch does not preserve the stack boundary";
                return std::nullopt;
            }
            {
                ScalarKernelInstruction value;
                value.op = ScalarKernelOp::Jump;
                appendInstruction(std::move(value));
                branchPatches.push_back(ScalarKernelBranchPatch{
                    kernel.instructions.size() - 1, pc,
                    static_cast<size_t>(instruction.target)});
            }
            break;
        case BytecodeOp::JumpIfFalse:
            if (!requireStack(
                    stack, 1,
                    "typed branch condition is missing",
                    failureReason)) {
                return std::nullopt;
            }
            if (stack.size() != 1 ||
                !isScalarStackValue(stack.back()) ||
                instruction.target < 0) {
                failureReason =
                    "typed conditional branch has an invalid stack boundary";
                return std::nullopt;
            }
            {
                ScalarKernelInstruction value;
                value.op = ScalarKernelOp::JumpIfFalse;
                value.left = stack.back().scalar;
                stack.pop_back();
                appendInstruction(std::move(value));
                branchPatches.push_back(ScalarKernelBranchPatch{
                    kernel.instructions.size() - 1, pc,
                    static_cast<size_t>(instruction.target)});
            }
            break;
        case BytecodeOp::ForBegin: {
            if (!requireStack(stack, 1,
                              "typed nested loop range is missing",
                              failureReason)) {
                return std::nullopt;
            }
            if (instruction.target < 0 ||
                static_cast<size_t>(instruction.target) >
                    region.bodyEndPc ||
                static_cast<size_t>(instruction.target) <= pc + 1) {
                failureReason =
                    "typed nested loop has invalid control boundaries";
                return std::nullopt;
            }

            const auto rangeValue = stack.back();
            stack.pop_back();
            if (rangeValue.isArray) {
                failureReason =
                    "typed nested loop range cannot be an array binding";
                return std::nullopt;
            }
            if (findArraySlot(kernel, instruction.operand)) {
                failureReason =
                    "typed nested loop cannot replace an array binding";
                return std::nullopt;
            }
            const size_t slot = findOrAddSlot(
                kernel, instruction.operand, variables);
            auto initializedBefore = kernel.initialized;
            kernel.initialized[slot] = true;

            ScalarKernelRange range;
            if (rangeValue.isRange) {
                range = rangeValue.range;
            } else {
                range.start = rangeValue.scalar;
                range.step = {ScalarKernelStorage::Literal, 0,
                              TypedScalar{1.0}};
                range.stop = rangeValue.scalar;
                range.singleValue = true;
            }

            ScalarKernelInstruction value;
            value.op = ScalarKernelOp::LoopBegin;
            value.destination = {ScalarKernelStorage::Slot, slot};
            value.left = range.start;
            value.right = range.stop;
            value.step = range.step;
            value.loopId = kernel.nestedLoopCount;
            value.singleValueRange = range.singleValue;
            appendInstruction(std::move(value));
            const size_t beginInstruction =
                kernel.instructions.size() - 1;
            loops.push_back(ScalarKernelCompileLoop{
                beginInstruction,
                static_cast<size_t>(instruction.target) - 1,
                slot,
                instruction.operand,
                std::move(initializedBefore)});
            ++kernel.nestedLoopCount;
            break;
        }
        case BytecodeOp::ForNext: {
            if (loops.empty() || loops.back().expectedLatchPc != pc ||
                loops.back().variable != instruction.operand) {
                failureReason =
                    "typed nested loop latch does not match its header";
                return std::nullopt;
            }
            auto loop = std::move(loops.back());
            loops.pop_back();

            ScalarKernelInstruction value;
            value.op = ScalarKernelOp::LoopNext;
            value.destination = {ScalarKernelStorage::Slot,
                                 loop.loopSlot};
            value.jumpTarget = loop.beginInstruction + 1;
            appendInstruction(std::move(value));
            kernel.instructions[loop.beginInstruction].jumpTarget =
                kernel.instructions.size();
            kernel.instructions[loop.beginInstruction].leafLoop =
                std::none_of(
                    kernel.instructions.begin() +
                        static_cast<std::ptrdiff_t>(
                            loop.beginInstruction + 1),
                    kernel.instructions.end() - 1,
                    [](const ScalarKernelInstruction& instruction) {
                        return instruction.op == ScalarKernelOp::LoopBegin ||
                               instruction.op == ScalarKernelOp::Jump ||
                               instruction.op ==
                                   ScalarKernelOp::JumpIfFalse;
                    });

            loop.initializedBefore.resize(kernel.initialized.size(), false);
            kernel.initialized = std::move(loop.initializedBefore);
            break;
        }
        default:
            failureReason =
                "typed region encountered an unsupported instruction";
            return std::nullopt;
        }
    }

    pcToKernel.back() = kernel.instructions.size();
    for (const auto& patch : branchPatches) {
        if (patch.targetPc <= patch.sourcePc ||
            patch.targetPc < region.bodyBeginPc ||
            patch.targetPc > region.bodyEndPc) {
            failureReason =
                "typed branch target escapes the scalar loop body";
            return std::nullopt;
        }
        const size_t target =
            pcToKernel[patch.targetPc - region.bodyBeginPc];
        if (target == std::numeric_limits<size_t>::max() ||
            target <= patch.instruction ||
            target > kernel.instructions.size()) {
            failureReason = "typed branch target is not forward and closed";
            return std::nullopt;
        }
        kernel.instructions[patch.instruction].jumpTarget = target;
    }

    if (!stack.empty()) {
        failureReason =
            "typed region body did not restore its stack boundary";
        return std::nullopt;
    }
    if (!loops.empty()) {
        failureReason = "typed region has an unterminated nested loop";
        return std::nullopt;
    }
    if (pendingSourceInstructionCount != 0) {
        failureReason =
            "typed region source instructions were not lowered";
        return std::nullopt;
    }
    if (kernel.nestedLoopCount != region.nestedLoopCount) {
        failureReason =
            "typed region nested loop contract does not match bytecode";
        return std::nullopt;
    }
    return kernel;
}

TypedScalar binaryResult(ScalarKernelOp operation, const TypedScalar& left,
                         const TypedScalar& right) {
    switch (operation) {
    case ScalarKernelOp::Add:
        return TypedScalar{left.value + right.value};
    case ScalarKernelOp::Subtract:
        return TypedScalar{left.value - right.value};
    case ScalarKernelOp::Multiply:
        return TypedScalar{left.value * right.value};
    case ScalarKernelOp::Divide:
        return TypedScalar{left.value / right.value};
    case ScalarKernelOp::Power:
        return TypedScalar{std::pow(left.value, right.value)};
    case ScalarKernelOp::Greater:
        return TypedScalar{left.value > right.value ? 1.0 : 0.0,
                           RuntimeNumericClass::Logical};
    case ScalarKernelOp::Less:
        return TypedScalar{left.value < right.value ? 1.0 : 0.0,
                           RuntimeNumericClass::Logical};
    case ScalarKernelOp::GreaterEqual:
        return TypedScalar{left.value >= right.value ? 1.0 : 0.0,
                           RuntimeNumericClass::Logical};
    case ScalarKernelOp::LessEqual:
        return TypedScalar{left.value <= right.value ? 1.0 : 0.0,
                           RuntimeNumericClass::Logical};
    case ScalarKernelOp::Equal:
        return TypedScalar{left.value == right.value ? 1.0 : 0.0,
                           RuntimeNumericClass::Logical};
    case ScalarKernelOp::NotEqual:
        return TypedScalar{left.value != right.value ? 1.0 : 0.0,
                           RuntimeNumericClass::Logical};
    case ScalarKernelOp::LogicalAnd:
        return TypedScalar{truthy(left.value) && truthy(right.value) ? 1.0
                                                                    : 0.0,
                           RuntimeNumericClass::Logical};
    case ScalarKernelOp::LogicalOr:
        return TypedScalar{truthy(left.value) || truthy(right.value) ? 1.0
                                                                    : 0.0,
                           RuntimeNumericClass::Logical};
    default:
        return TypedScalar{};
    }
}

double mathResult(ScalarKernelOp operation, double value) {
    switch (operation) {
    case ScalarKernelOp::Absolute:
        return std::fabs(value);
    case ScalarKernelOp::ArcCosine:
        return std::acos(value);
    case ScalarKernelOp::ArcSine:
        return std::asin(value);
    case ScalarKernelOp::ArcTangent:
        return std::atan(value);
    case ScalarKernelOp::Cosine:
        return std::cos(value);
    case ScalarKernelOp::Exponential:
        return std::exp(value);
    case ScalarKernelOp::Logarithm:
        return std::log(value);
    case ScalarKernelOp::Sine:
        return std::sin(value);
    case ScalarKernelOp::SquareRoot:
        return std::sqrt(value);
    case ScalarKernelOp::Tangent:
        return std::tan(value);
    default:
        return value;
    }
}

TypedScalar readOperand(const ScalarKernelOperand& operand,
                        const ScalarKernel& kernel,
                        const std::vector<TypedScalar>& registers) {
    switch (operand.storage) {
    case ScalarKernelStorage::Slot:
        return kernel.slots[operand.index];
    case ScalarKernelStorage::Register:
        return registers[operand.index];
    case ScalarKernelStorage::Literal:
        return operand.literal;
    }
    return {};
}

void writeDestination(const ScalarKernelDestination& destination,
                       const TypedScalar& value, ScalarKernel& kernel,
                       std::vector<TypedScalar>& registers,
                       std::vector<bool>& written) {
    if (destination.storage == ScalarKernelStorage::Slot) {
        kernel.slots[destination.index] = value;
        written[destination.index] = true;
    } else {
        registers[destination.index] = value;
    }
}

std::optional<size_t> checkedLinearArrayOffset(
    double oneBasedIndex, const TypedNumericArray& array,
    std::string& failureReason) {
    const auto index =
        checkedRuntimeNonnegativeInteger(oneBasedIndex);
    if (!index || *index == 0) {
        failureReason =
            "typed linear index must be a finite positive integer";
        return std::nullopt;
    }
    if (*index > array.elements.size()) {
        failureReason =
            "typed linear index exceeds the preallocated array bounds";
        return std::nullopt;
    }
    return *index - 1;
}

bool executeKernelInstruction(
    const ScalarKernelInstruction& instruction, ScalarKernel& kernel,
    std::vector<TypedScalar>& registers, std::vector<bool>& written,
    std::vector<bool>& writtenArrays, std::string& failureReason) {
    if (instruction.op == ScalarKernelOp::Discard ||
        instruction.op == ScalarKernelOp::Jump ||
        instruction.op == ScalarKernelOp::JumpIfFalse ||
        instruction.op == ScalarKernelOp::LoopBegin ||
        instruction.op == ScalarKernelOp::LoopNext) {
        return true;
    }

    TypedScalar result;
    switch (instruction.op) {
    case ScalarKernelOp::Copy:
        result = readOperand(instruction.left, kernel, registers);
        break;
    case ScalarKernelOp::UnaryPlus:
        result = readOperand(instruction.left, kernel, registers);
        result.numericClass = RuntimeNumericClass::Double;
        break;
    case ScalarKernelOp::UnaryMinus:
        result = TypedScalar{
            -readOperand(instruction.left, kernel, registers).value};
        break;
    case ScalarKernelOp::LogicalNot:
        result = TypedScalar{
            truthy(readOperand(instruction.left, kernel, registers).value)
                ? 0.0
                : 1.0,
            RuntimeNumericClass::Logical};
        break;
    case ScalarKernelOp::Add:
    case ScalarKernelOp::Subtract:
    case ScalarKernelOp::Multiply:
    case ScalarKernelOp::Divide:
    case ScalarKernelOp::Power:
    case ScalarKernelOp::Greater:
    case ScalarKernelOp::Less:
    case ScalarKernelOp::GreaterEqual:
    case ScalarKernelOp::LessEqual:
    case ScalarKernelOp::Equal:
    case ScalarKernelOp::NotEqual:
    case ScalarKernelOp::LogicalAnd:
    case ScalarKernelOp::LogicalOr:
        {
            const TypedScalar left = readOperand(
                instruction.left, kernel, registers);
            const TypedScalar right = readOperand(
                instruction.right, kernel, registers);
            if (instruction.op == ScalarKernelOp::Power &&
                left.value < 0.0 && std::isfinite(right.value) &&
                std::floor(right.value) != right.value) {
                failureReason =
                    "typed power requires a complex result";
                return false;
            }
            result = binaryResult(instruction.op, left, right);
        }
        break;
    case ScalarKernelOp::Absolute:
    case ScalarKernelOp::ArcCosine:
    case ScalarKernelOp::ArcSine:
    case ScalarKernelOp::ArcTangent:
    case ScalarKernelOp::Cosine:
    case ScalarKernelOp::Exponential:
    case ScalarKernelOp::Logarithm:
    case ScalarKernelOp::Sine:
    case ScalarKernelOp::SquareRoot:
    case ScalarKernelOp::Tangent:
        {
            const double input = readOperand(
                instruction.left, kernel, registers).value;
            const bool inverseDomain =
                (instruction.op == ScalarKernelOp::ArcCosine ||
                 instruction.op == ScalarKernelOp::ArcSine) &&
                std::fabs(input) > 1.0;
            const bool negativeDomain =
                (instruction.op == ScalarKernelOp::Logarithm ||
                 instruction.op == ScalarKernelOp::SquareRoot) &&
                input < 0.0;
            if (inverseDomain || negativeDomain) {
                failureReason =
                    "typed math builtin requires a complex result";
                return false;
            }
            result = TypedScalar{
                mathResult(instruction.op, input)};
        }
        break;
    case ScalarKernelOp::LoadArrayElement: {
        if (instruction.arraySlot >= kernel.arrays.size()) {
            failureReason =
                "typed kernel references an invalid array slot";
            return false;
        }
        const auto offset = checkedLinearArrayOffset(
            readOperand(instruction.left, kernel, registers).value,
            kernel.arrays[instruction.arraySlot], failureReason);
        if (!offset) {
            return false;
        }
        result = TypedScalar{
            kernel.arrays[instruction.arraySlot].elements[*offset]};
        break;
    }
    case ScalarKernelOp::StoreArrayElement: {
        if (instruction.arraySlot >= kernel.arrays.size()) {
            failureReason =
                "typed kernel references an invalid array slot";
            return false;
        }
        const auto offset = checkedLinearArrayOffset(
            readOperand(instruction.left, kernel, registers).value,
            kernel.arrays[instruction.arraySlot], failureReason);
        if (!offset) {
            return false;
        }
        kernel.arrays[instruction.arraySlot].elements[*offset] =
            readOperand(instruction.right, kernel, registers).value;
        writtenArrays[instruction.arraySlot] = true;
        return true;
    }
    case ScalarKernelOp::Discard:
    case ScalarKernelOp::Jump:
    case ScalarKernelOp::JumpIfFalse:
    case ScalarKernelOp::LoopBegin:
    case ScalarKernelOp::LoopNext:
        return true;
    }
    writeDestination(instruction.destination, result, kernel, registers,
                     written);
    return true;
}

bool executeKernelSpan(ScalarKernel& kernel, size_t begin, size_t end,
                       std::vector<TypedScalar>& registers,
                       std::vector<bool>& written,
                       std::vector<bool>& writtenArrays,
                       ScalarKernelExecutionCounters& counters,
                       std::string& failureReason) {
    size_t pc = begin;
    while (pc < end) {
        const auto& instruction = kernel.instructions[pc];
        if (instruction.op == ScalarKernelOp::LoopNext) {
            failureReason =
                "typed nested kernel reached an unmatched loop latch";
            return false;
        }

        ++counters.kernelInstructions;
        counters.sourceInstructions += instruction.sourceInstructionCount;
        if (instruction.op == ScalarKernelOp::Jump ||
            instruction.op == ScalarKernelOp::JumpIfFalse) {
            if (instruction.jumpTarget <= pc ||
                instruction.jumpTarget > end) {
                failureReason =
                    "typed kernel branch target escapes its control span";
                return false;
            }
            if (instruction.op == ScalarKernelOp::Jump ||
                !truthy(readOperand(instruction.left, kernel,
                                    registers)
                            .value)) {
                pc = instruction.jumpTarget;
            } else {
                ++pc;
            }
            continue;
        }
        if (instruction.op != ScalarKernelOp::LoopBegin) {
            if (!executeKernelInstruction(
                    instruction, kernel, registers, written,
                    writtenArrays, failureReason)) {
                return false;
            }
            ++pc;
            continue;
        }

        if (instruction.jumpTarget <= pc + 1 ||
            instruction.jumpTarget > end ||
            kernel.instructions[instruction.jumpTarget - 1].op !=
                ScalarKernelOp::LoopNext) {
            failureReason =
                "typed nested kernel has invalid loop boundaries";
            return false;
        }

        const size_t bodyBegin = pc + 1;
        const size_t bodyEnd = instruction.jumpTarget - 1;
        const auto& latch = kernel.instructions[bodyEnd];
        const double start =
            readOperand(instruction.left, kernel, registers).value;

        const auto executeIteration = [&](double value) {
            kernel.slots[instruction.destination.index] =
                TypedScalar{value};
            written[instruction.destination.index] = true;
            ++counters.nestedIterations;
            if (instruction.leafLoop) {
                for (size_t bodyPc = bodyBegin; bodyPc < bodyEnd;
                     ++bodyPc) {
                    const auto& bodyInstruction =
                        kernel.instructions[bodyPc];
                    ++counters.kernelInstructions;
                    counters.sourceInstructions +=
                        bodyInstruction.sourceInstructionCount;
                    if (!executeKernelInstruction(
                            bodyInstruction, kernel, registers, written,
                            writtenArrays, failureReason)) {
                        return false;
                    }
                }
            } else if (!executeKernelSpan(
                           kernel, bodyBegin, bodyEnd, registers, written,
                           writtenArrays, counters, failureReason)) {
                return false;
            }
            counters.sourceInstructions += latch.sourceInstructionCount;
            return true;
        };

        if (instruction.singleValueRange) {
            if (!executeIteration(start)) {
                return false;
            }
            pc = instruction.jumpTarget;
            continue;
        }

        const double step =
            readOperand(instruction.step, kernel, registers).value;
        const double stop =
            readOperand(instruction.right, kernel, registers).value;
        const auto range = runtimePlanColonRange(start, step, stop);
        if (!range.succeeded) {
            failureReason = "typed nested " + range.error;
            return false;
        }
        for (double value = range.start;
             runtimeColonRangeContains(range, value);) {
            if (!executeIteration(value)) {
                return false;
            }
            const double next = value + range.step;
            if (next == value) {
                break;
            }
            value = next;
        }
        pc = instruction.jumpTarget;
    }
    return true;
}

} // namespace

ScalarTypedRegionExecutor::ScalarTypedRegionExecutor()
    : builtinRegistry_(defaultBuiltinRegistry()) {}

ScalarTypedRegionExecutor::ScalarTypedRegionExecutor(
    std::shared_ptr<const BuiltinRegistry> builtinRegistry)
    : builtinRegistry_(builtinRegistry
                           ? std::move(builtinRegistry)
                           : defaultBuiltinRegistry()) {}

TypedRegionExecutionResult ScalarTypedRegionExecutor::execute(
    const BytecodeProgram& program, const BytecodeRegionContract& region,
    const RuntimeValue& loopRange,
    const RuntimeWorkspace& variables,
    TypedRegionBackend backend) const {
    BytecodeRegionAnalyzer analyzer(builtinRegistry_);
    const auto expected = analyzer.analyze(
        program, "hot-loop", region.beginPc, {});
    if (expected.fallbackKind ==
            RuntimeFallbackKind::InvalidContract) {
        return fallback(
            RuntimeFallbackKind::InvalidContract,
            expected.reason.empty()
                ? "bytecode program validation failed"
                : expected.reason);
    }
    if (!bytecodeRegionContractsEquivalent(region, expected)) {
        return fallback(
            RuntimeFallbackKind::InvalidContract,
            "typed region contract does not match its bytecode program");
    }
    return executeValidated(program, expected, loopRange, variables,
                            backend);
}

TypedRegionExecutionResult
ScalarTypedRegionExecutor::executeValidated(
    const BytecodeProgram& program,
    const BytecodeRegionContract& region,
    const RuntimeValue& loopRange,
    const RuntimeWorkspace& variables,
    TypedRegionBackend backend) const {
    if (!region.available || !region.closed ||
        !region.eligibleForTypedExecution) {
        return fallback(
            region.fallbackKind == RuntimeFallbackKind::None
                ? RuntimeFallbackKind::InvalidContract
                : region.fallbackKind,
            "typed region contract is not executable");
    }
    if (region.beginPc >= program.instructions.size() ||
        region.endPc > program.instructions.size() ||
        region.bodyBeginPc > region.bodyEndPc ||
        region.bodyEndPc >= region.endPc) {
        return fallback(
            RuntimeFallbackKind::InvalidContract,
            "typed region contract has invalid PC boundaries");
    }

    const auto& header = program.instructions[region.beginPc];
    if (header.op != BytecodeOp::ForBegin || header.operand.empty()) {
        return fallback(
            RuntimeFallbackKind::InvalidContract,
            "typed region entry is not a named for loop");
    }

    const auto values = loopValues(loopRange);
    if (!values) {
        return fallback(RuntimeFallbackKind::UnsupportedRange,
                        "typed loop range is not numeric");
    }

    for (const auto& input : region.inputs) {
        const auto variable = variables.find(input);
        if (variable == variables.end()) {
            return fallback(
                RuntimeFallbackKind::MissingInput,
                "typed region input is unavailable: " + input);
        }
        const bool supportedScalar =
            variable->second.kind == RuntimeValueKind::Number &&
            variable->second.numericClass == RuntimeNumericClass::Double &&
            !variable->second.numericComplex;
        if (!supportedScalar &&
            !isDirectLinearDoubleArray(variable->second)) {
            return fallback(
                RuntimeFallbackKind::UnsupportedInput,
                "typed region input is not a supported double scalar or linear array: " +
                input);
        }
    }

    std::string compileFailure;
    auto kernel = compileKernel(
        program, region, variables, *builtinRegistry_,
        compileFailure);
    if (!kernel) {
        return fallback(RuntimeFallbackKind::KernelRejected,
                        std::move(compileFailure));
    }

    std::string nativeFallbackReason;
    RuntimeFallbackKind nativeFallbackCode =
        RuntimeFallbackKind::None;
    if (backend != TypedRegionBackend::Portable) {
        std::vector<double> nativeOuterValues;
        nativeOuterValues.reserve(values->size());
        for (size_t index = 0; index < values->size(); ++index) {
            nativeOuterValues.push_back((*values)[index]);
        }

        ScalarKernel nativeKernel = *kernel;
        auto native = executeNativeScalarKernel(
            nativeKernel, nativeOuterValues.data(),
            nativeOuterValues.size());
        if (native.status == NativeScalarJitStatus::Executed) {
            RuntimeWorkspace workingVariables = variables;
            for (size_t slot = 0;
                 slot < nativeKernel.slotNames.size(); ++slot) {
                if (native.writtenSlots[slot] != 0) {
                    workingVariables[nativeKernel.slotNames[slot]] =
                        numberValue(nativeKernel.slots[slot]);
                }
            }
            for (size_t slot = 0;
                 slot < nativeKernel.arraySlotNames.size(); ++slot) {
                if (slot < native.writtenArrays.size() &&
                    native.writtenArrays[slot] != 0) {
                    workingVariables[
                        nativeKernel.arraySlotNames[slot]] =
                        arrayValue(nativeKernel.arrays[slot]);
                }
            }

            TypedRegionExecutionResult result;
            result.status = TypedRegionExecutionStatus::Executed;
            result.variables = std::move(workingVariables);
            result.iterationCount = nativeOuterValues.size();
            result.nestedIterationCount =
                native.counters.nestedIterations;
            result.executedInstructionCount =
                native.counters.sourceInstructions;
            result.executedKernelInstructionCount =
                native.counters.kernelInstructions;
            result.backend = TypedRegionBackend::Native;
            result.nativeCompiled = native.compiled;
            result.nativeCacheHit = native.cacheHit;
            result.nativeCacheStored = native.cacheStored;
            result.nativeCacheBypassed = native.cacheBypassed;
            result.nativeCacheEvictionCount =
                native.cacheEvictionCount;
            result.nativeCacheEvictedCodeBytes =
                native.cacheEvictedCodeBytes;
            result.nativeCodeSize = native.codeSize;
            result.nativePlatform = nativeScalarJitPlatform();
            result.reason = std::move(native.reason);
            return result;
        }
        if (backend == TypedRegionBackend::Native) {
            const RuntimeFallbackKind kind =
                nativeFallbackKind(native.status);
            auto result = fallback(kind, std::move(native.reason));
            result.backend = TypedRegionBackend::Native;
            result.nativePlatform = nativeScalarJitPlatform();
            result.nativeFallbackKind = kind;
            result.nativeFallbackReason = result.reason;
            return result;
        }
        nativeFallbackCode = nativeFallbackKind(native.status);
        nativeFallbackReason = std::move(native.reason);
    }

    std::vector<TypedScalar> registers(kernel->registerCount);
    std::vector<bool> written(kernel->slotNames.size(), false);
    std::vector<bool> writtenArrays(
        kernel->arraySlotNames.size(), false);
    ScalarKernelExecutionCounters counters;
    for (size_t index = 0; index < values->size(); ++index) {
        kernel->slots[kernel->loopSlot] = TypedScalar{(*values)[index]};
        written[kernel->loopSlot] = true;
        if (!executeKernelSpan(*kernel, 0, kernel->instructions.size(),
                               registers, written, writtenArrays, counters,
                               compileFailure)) {
            auto result = fallback(RuntimeFallbackKind::RuntimeFailed,
                                   std::move(compileFailure));
            result.nativeFallbackKind = nativeFallbackCode;
            result.nativeFallbackReason =
                std::move(nativeFallbackReason);
            return result;
        }
    }
    RuntimeWorkspace workingVariables = variables;
    for (size_t slot = 0; slot < kernel->slotNames.size(); ++slot) {
        if (written[slot]) {
            workingVariables[kernel->slotNames[slot]] =
                numberValue(kernel->slots[slot]);
        }
    }
    for (size_t slot = 0; slot < kernel->arraySlotNames.size(); ++slot) {
        if (writtenArrays[slot]) {
            workingVariables[kernel->arraySlotNames[slot]] =
                arrayValue(kernel->arrays[slot]);
        }
    }

    TypedRegionExecutionResult result;
    result.status = TypedRegionExecutionStatus::Executed;
    result.variables = std::move(workingVariables);
    result.iterationCount = values->size();
    result.nestedIterationCount = counters.nestedIterations;
    result.executedInstructionCount = counters.sourceInstructions;
    result.executedKernelInstructionCount = counters.kernelInstructions;
    result.backend = TypedRegionBackend::Portable;
    result.nativeFallbackKind = nativeFallbackCode;
    result.nativeFallbackReason = std::move(nativeFallbackReason);
    if (!kernel->arrays.empty()) {
        result.reason = kernel->nestedLoopCount == 0
                            ? "predecoded linear-array scalar kernel executed"
                            : "predecoded nested linear-array scalar kernel executed";
    } else {
        result.reason = kernel->nestedLoopCount == 0
                            ? "predecoded scalar kernel executed"
                            : "predecoded nested scalar kernel executed";
    }
    return result;
}

} // namespace mparser
