#include "mparser/typed_region_executor.h"
#include "mparser/runtime_math.h"
#include "mparser/runtime_shape.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser {
namespace {

struct TypedScalar {
    double value = 0.0;
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
};

enum class ScalarKernelOp {
    Copy,
    UnaryPlus,
    UnaryMinus,
    LogicalNot,
    Add,
    Subtract,
    Multiply,
    Divide,
    Power,
    Greater,
    Less,
    GreaterEqual,
    LessEqual,
    Equal,
    NotEqual,
    LogicalAnd,
    LogicalOr,
    Absolute,
    ArcCosine,
    ArcSine,
    ArcTangent,
    Cosine,
    Exponential,
    Logarithm,
    Sine,
    SquareRoot,
    Tangent,
};

enum class ScalarKernelStorage {
    Slot,
    Register,
    Literal,
};

struct ScalarKernelOperand {
    ScalarKernelStorage storage = ScalarKernelStorage::Literal;
    size_t index = 0;
    TypedScalar literal;
};

struct ScalarKernelDestination {
    ScalarKernelStorage storage = ScalarKernelStorage::Register;
    size_t index = 0;
};

struct ScalarKernelInstruction {
    ScalarKernelOp op = ScalarKernelOp::Copy;
    ScalarKernelDestination destination;
    ScalarKernelOperand left;
    ScalarKernelOperand right;
};

struct ScalarKernel {
    std::vector<ScalarKernelInstruction> instructions;
    std::vector<std::string> slotNames;
    std::vector<TypedScalar> slots;
    std::vector<bool> initialized;
    std::vector<bool> written;
    size_t loopSlot = 0;
    size_t registerCount = 0;
    size_t sourceInstructionCount = 0;
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

std::optional<ScalarKernelOp> mathOperation(std::string_view name) {
    if (name == "abs") {
        return ScalarKernelOp::Absolute;
    }
    if (name == "acos") {
        return ScalarKernelOp::ArcCosine;
    }
    if (name == "asin") {
        return ScalarKernelOp::ArcSine;
    }
    if (name == "atan") {
        return ScalarKernelOp::ArcTangent;
    }
    if (name == "cos") {
        return ScalarKernelOp::Cosine;
    }
    if (name == "exp") {
        return ScalarKernelOp::Exponential;
    }
    if (name == "log") {
        return ScalarKernelOp::Logarithm;
    }
    if (name == "sin") {
        return ScalarKernelOp::Sine;
    }
    if (name == "sqrt") {
        return ScalarKernelOp::SquareRoot;
    }
    if (name == "tan") {
        return ScalarKernelOp::Tangent;
    }
    return std::nullopt;
}

std::optional<LoopRangeView> loopValues(const RuntimeValue& range) {
    if (range.numericClass != RuntimeNumericClass::Double) {
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

TypedRegionExecutionResult fallback(std::string reason) {
    TypedRegionExecutionResult result;
    result.reason = std::move(reason);
    return result;
}

size_t findOrAddSlot(ScalarKernel& kernel, std::string_view name,
                     const std::map<std::string, RuntimeValue>& variables) {
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
    kernel.written.push_back(false);

    const auto variable = variables.find(kernel.slotNames.back());
    if (variable != variables.end() &&
        variable->second.kind == RuntimeValueKind::Number) {
        kernel.slots.back() = TypedScalar{variable->second.number,
                                          variable->second.numericClass};
        kernel.initialized.back() = true;
    }
    return slot;
}

bool requireStack(const std::vector<ScalarKernelOperand>& stack,
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
    const std::map<std::string, RuntimeValue>& variables,
    std::string& failureReason) {
    ScalarKernel kernel;
    kernel.instructions.reserve(region.bodyEndPc - region.bodyBeginPc);
    kernel.sourceInstructionCount = region.bodyEndPc - region.bodyBeginPc;

    const auto& header = program.instructions[region.beginPc];
    kernel.loopSlot = findOrAddSlot(kernel, header.operand, variables);
    kernel.initialized[kernel.loopSlot] = true;
    kernel.slots[kernel.loopSlot] = TypedScalar{};
    kernel.written[kernel.loopSlot] = true;

    std::vector<ScalarKernelOperand> stack;
    stack.reserve(region.bodyEndPc - region.bodyBeginPc);
    const auto appendUnary = [&](ScalarKernelOp operation,
                                 ScalarKernelOperand operand) {
        const size_t result = kernel.registerCount++;
        kernel.instructions.push_back(
            {operation,
             {ScalarKernelStorage::Register, result},
             operand,
             {}});
        return ScalarKernelOperand{ScalarKernelStorage::Register, result,
                                   {}};
    };
    const auto appendBinary = [&](ScalarKernelOp operation,
                                  ScalarKernelOperand left,
                                  ScalarKernelOperand right) {
        const size_t result = kernel.registerCount++;
        kernel.instructions.push_back(
            {operation,
             {ScalarKernelStorage::Register, result},
             left,
             right});
        return ScalarKernelOperand{ScalarKernelStorage::Register, result,
                                   {}};
    };

    for (size_t pc = region.bodyBeginPc; pc < region.bodyEndPc; ++pc) {
        const auto& instruction = program.instructions[pc];
        switch (instruction.op) {
        case BytecodeOp::LoadName: {
            if (instruction.binding.kind == BindingKind::Builtin &&
                isRuntimePureUnaryMathBuiltin(instruction.operand)) {
                break;
            }
            const size_t slot =
                findOrAddSlot(kernel, instruction.operand, variables);
            if (!kernel.initialized[slot]) {
                failureReason = "typed region load is unavailable: " +
                                instruction.operand;
                return std::nullopt;
            }
            stack.push_back(
                {ScalarKernelStorage::Slot, slot, {}});
            break;
        }
        case BytecodeOp::LoadLiteral: {
            const auto value = parseNumber(instruction.operand);
            if (!value) {
                failureReason = "typed region literal is not numeric";
                return std::nullopt;
            }
            stack.push_back({ScalarKernelStorage::Literal, 0,
                             TypedScalar{*value}});
            break;
        }
        case BytecodeOp::StoreName: {
            if (!requireStack(stack, 1,
                              "typed region stack underflow at store",
                              failureReason)) {
                return std::nullopt;
            }
            const size_t slot =
                findOrAddSlot(kernel, instruction.operand, variables);
            const auto source = stack.back();
            stack.pop_back();
            if (source.storage == ScalarKernelStorage::Register &&
                !kernel.instructions.empty() &&
                kernel.instructions.back().destination.storage ==
                    ScalarKernelStorage::Register &&
                kernel.instructions.back().destination.index ==
                    source.index) {
                kernel.instructions.back().destination =
                    {ScalarKernelStorage::Slot, slot};
            } else {
                kernel.instructions.push_back(
                    {ScalarKernelOp::Copy,
                     {ScalarKernelStorage::Slot, slot},
                     source,
                     {}});
            }
            kernel.initialized[slot] = true;
            kernel.written[slot] = true;
            break;
        }
        case BytecodeOp::UnaryOp: {
            if (!requireStack(
                    stack, 1,
                    "typed region stack underflow at unary operation",
                    failureReason)) {
                return std::nullopt;
            }
            const auto operation = unaryOperation(instruction.operand);
            if (!operation) {
                failureReason =
                    "typed region unary operation is unsupported";
                return std::nullopt;
            }
            stack.back() = appendUnary(*operation, stack.back());
            break;
        }
        case BytecodeOp::BinaryOp: {
            if (!requireStack(
                    stack, 2,
                    "typed region stack underflow at binary operation",
                    failureReason)) {
                return std::nullopt;
            }
            const auto operation = binaryOperation(instruction.operand);
            if (!operation) {
                failureReason =
                    "typed region binary operation is unsupported";
                return std::nullopt;
            }
            const auto right = stack.back();
            stack.pop_back();
            const auto left = stack.back();
            stack.back() = appendBinary(*operation, left, right);
            break;
        }
        case BytecodeOp::CallOrIndex: {
            if (instruction.binding.kind != BindingKind::Builtin ||
                instruction.operandCount != 1 ||
                instruction.resultCount != 1) {
                failureReason =
                    "typed region call is not a pure unary builtin";
                return std::nullopt;
            }
            const auto operation = mathOperation(instruction.calleeName);
            if (!operation) {
                failureReason = "typed region builtin call is unsupported";
                return std::nullopt;
            }
            if (!requireStack(
                    stack, 1,
                    "typed region stack underflow at builtin call",
                    failureReason)) {
                return std::nullopt;
            }
            stack.back() = appendUnary(*operation, stack.back());
            break;
        }
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
            break;
        case BytecodeOp::Pop:
            if (!requireStack(stack, 1,
                              "typed region stack underflow at pop",
                              failureReason)) {
                return std::nullopt;
            }
            stack.pop_back();
            break;
        default:
            failureReason =
                "typed region encountered an unsupported instruction";
            return std::nullopt;
        }
    }

    if (!stack.empty()) {
        failureReason =
            "typed region body did not restore its stack boundary";
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
                      std::vector<TypedScalar>& registers) {
    if (destination.storage == ScalarKernelStorage::Slot) {
        kernel.slots[destination.index] = value;
    } else {
        registers[destination.index] = value;
    }
}

void executeKernelInstruction(
    const ScalarKernelInstruction& instruction, ScalarKernel& kernel,
    std::vector<TypedScalar>& registers) {
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
        result = binaryResult(
            instruction.op,
            readOperand(instruction.left, kernel, registers),
            readOperand(instruction.right, kernel, registers));
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
        result = TypedScalar{mathResult(
            instruction.op,
            readOperand(instruction.left, kernel, registers).value)};
        break;
    }
    writeDestination(instruction.destination, result, kernel, registers);
}

} // namespace

TypedRegionExecutionResult ScalarTypedRegionExecutor::execute(
    const BytecodeProgram& program, const BytecodeRegionContract& region,
    const RuntimeValue& loopRange,
    const std::map<std::string, RuntimeValue>& variables) const {
    if (!region.available || !region.closed ||
        !region.eligibleForTypedExecution) {
        return fallback("typed region contract is not executable");
    }
    if (region.beginPc >= program.instructions.size() ||
        region.endPc > program.instructions.size() ||
        region.bodyBeginPc > region.bodyEndPc ||
        region.bodyEndPc >= region.endPc) {
        return fallback("typed region contract has invalid PC boundaries");
    }

    const auto& header = program.instructions[region.beginPc];
    if (header.op != BytecodeOp::ForBegin || header.operand.empty()) {
        return fallback("typed region entry is not a named for loop");
    }

    const auto values = loopValues(loopRange);
    if (!values) {
        return fallback("typed loop range is not numeric");
    }

    for (const auto& input : region.inputs) {
        const auto variable = variables.find(input);
        if (variable == variables.end()) {
            return fallback("typed region input is unavailable: " + input);
        }
        if (variable->second.kind != RuntimeValueKind::Number ||
            variable->second.numericClass != RuntimeNumericClass::Double) {
            return fallback("typed region input is not scalar numeric: " +
                            input);
        }
    }

    std::string compileFailure;
    auto kernel = compileKernel(program, region, variables, compileFailure);
    if (!kernel) {
        return fallback(std::move(compileFailure));
    }

    std::vector<TypedScalar> registers(kernel->registerCount);
    for (size_t index = 0; index < values->size(); ++index) {
        kernel->slots[kernel->loopSlot] = TypedScalar{(*values)[index]};
        for (const auto& instruction : kernel->instructions) {
            executeKernelInstruction(instruction, *kernel, registers);
        }
    }
    std::map<std::string, RuntimeValue> workingVariables = variables;
    if (values->size() != 0) {
        for (size_t slot = 0; slot < kernel->slotNames.size(); ++slot) {
            if (kernel->written[slot]) {
                workingVariables[kernel->slotNames[slot]] =
                    numberValue(kernel->slots[slot]);
            }
        }
    }

    TypedRegionExecutionResult result;
    result.status = TypedRegionExecutionStatus::Executed;
    result.variables = std::move(workingVariables);
    result.iterationCount = values->size();
    result.executedInstructionCount =
        values->size() * kernel->sourceInstructionCount;
    result.executedKernelInstructionCount =
        values->size() * kernel->instructions.size();
    result.reason = "predecoded scalar kernel executed";
    return result;
}

} // namespace mparser
