#include "mparser/typed_region_executor.h"
#include "mparser/runtime_shape.h"

#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser {
namespace {

RuntimeValue numberValue(double value) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Number;
    result.number = value;
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

std::optional<double> applyUnary(std::string_view operation, double value) {
    if (operation == "+") {
        return value;
    }
    if (operation == "-") {
        return -value;
    }
    if (operation == "~") {
        return value != 0.0 && !std::isnan(value) ? 0.0 : 1.0;
    }
    return std::nullopt;
}

std::optional<double> applyBinary(std::string_view operation, double left,
                                  double right) {
    if (operation == "+") {
        return left + right;
    }
    if (operation == "-") {
        return left - right;
    }
    if (operation == "*" || operation == ".*") {
        return left * right;
    }
    if (operation == "/" || operation == "./") {
        return left / right;
    }
    if (operation == "^" || operation == ".^") {
        return std::pow(left, right);
    }
    if (operation == ">") {
        return left > right ? 1.0 : 0.0;
    }
    if (operation == "<") {
        return left < right ? 1.0 : 0.0;
    }
    if (operation == ">=") {
        return left >= right ? 1.0 : 0.0;
    }
    if (operation == "<=") {
        return left <= right ? 1.0 : 0.0;
    }
    if (operation == "==") {
        return left == right ? 1.0 : 0.0;
    }
    if (operation == "~=") {
        return left != right ? 1.0 : 0.0;
    }
    if (operation == "&" || operation == "&&") {
        return left != 0.0 && right != 0.0 ? 1.0 : 0.0;
    }
    if (operation == "|" || operation == "||") {
        return left != 0.0 || right != 0.0 ? 1.0 : 0.0;
    }
    return std::nullopt;
}

std::optional<std::vector<double>> loopValues(const RuntimeValue& range) {
    if (range.kind == RuntimeValueKind::Number) {
        return std::vector<double>{range.number};
    }
    if (range.kind == RuntimeValueKind::Vector ||
        range.kind == RuntimeValueKind::Matrix) {
        return range.elements;
    }
    return std::nullopt;
}

TypedRegionExecutionResult fallback(std::string reason) {
    TypedRegionExecutionResult result;
    result.reason = std::move(reason);
    return result;
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
        if (variable->second.kind != RuntimeValueKind::Number) {
            return fallback("typed region input is not scalar numeric: " +
                            input);
        }
    }

    std::map<std::string, RuntimeValue> workingVariables = variables;
    size_t instructionCount = 0;
    for (double loopValue : *values) {
        workingVariables[header.operand] = numberValue(loopValue);
        std::vector<double> stack;

        for (size_t pc = region.bodyBeginPc; pc < region.bodyEndPc; ++pc) {
            const auto& instruction = program.instructions[pc];
            ++instructionCount;
            switch (instruction.op) {
            case BytecodeOp::LoadName: {
                const auto variable =
                    workingVariables.find(instruction.operand);
                if (variable == workingVariables.end()) {
                    return fallback(
                        "typed region load is unavailable: " +
                        instruction.operand);
                }
                if (variable->second.kind != RuntimeValueKind::Number) {
                    return fallback(
                        "typed region load is not scalar numeric: " +
                        instruction.operand);
                }
                stack.push_back(variable->second.number);
                break;
            }
            case BytecodeOp::LoadLiteral: {
                const auto value = parseNumber(instruction.operand);
                if (!value) {
                    return fallback("typed region literal is not numeric");
                }
                stack.push_back(*value);
                break;
            }
            case BytecodeOp::StoreName:
                if (stack.empty()) {
                    return fallback("typed region stack underflow at store");
                }
                workingVariables[instruction.operand] =
                    numberValue(stack.back());
                stack.pop_back();
                break;
            case BytecodeOp::UnaryOp: {
                if (stack.empty()) {
                    return fallback(
                        "typed region stack underflow at unary operation");
                }
                const auto value =
                    applyUnary(instruction.operand, stack.back());
                if (!value) {
                    return fallback(
                        "typed region unary operation is unsupported");
                }
                stack.back() = *value;
                break;
            }
            case BytecodeOp::BinaryOp: {
                if (stack.size() < 2) {
                    return fallback(
                        "typed region stack underflow at binary operation");
                }
                const double right = stack.back();
                stack.pop_back();
                const double left = stack.back();
                stack.pop_back();
                const auto value =
                    applyBinary(instruction.operand, left, right);
                if (!value) {
                    return fallback(
                        "typed region binary operation is unsupported");
                }
                stack.push_back(*value);
                break;
            }
            case BytecodeOp::PostfixOp:
                if (stack.empty()) {
                    return fallback(
                        "typed region stack underflow at postfix operation");
                }
                if (instruction.operand != "'") {
                    return fallback(
                        "typed region postfix operation is unsupported");
                }
                break;
            case BytecodeOp::Pop:
                if (stack.empty()) {
                    return fallback("typed region stack underflow at pop");
                }
                stack.pop_back();
                break;
            default:
                return fallback(
                    "typed region encountered an unsupported instruction");
            }
        }

        if (!stack.empty()) {
            return fallback(
                "typed region body did not restore its stack boundary");
        }
    }

    TypedRegionExecutionResult result;
    result.status = TypedRegionExecutionStatus::Executed;
    result.variables = std::move(workingVariables);
    result.iterationCount = values->size();
    result.executedInstructionCount = instructionCount;
    result.reason = "typed scalar loop executed";
    return result;
}

} // namespace mparser
