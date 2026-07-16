#pragma once

#include "mparser/interpreter.h"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace mparser {

struct TypedScalar {
    double value = 0.0;
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
};

struct TypedNumericArray {
    RuntimeValueKind kind = RuntimeValueKind::Matrix;
    std::vector<double> elements;
    std::vector<size_t> dimensions;
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
    LoadArrayElement,
    StoreArrayElement,
    Discard,
    Jump,
    JumpIfFalse,
    LoopBegin,
    LoopNext,
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
    ScalarKernelOperand step;
    size_t jumpTarget = 0;
    size_t sourceInstructionCount = 0;
    size_t arraySlot = std::numeric_limits<size_t>::max();
    size_t loopId = std::numeric_limits<size_t>::max();
    bool singleValueRange = false;
    bool leafLoop = false;
};

struct ScalarKernelExecutionCounters {
    size_t sourceInstructions = 0;
    size_t kernelInstructions = 0;
    size_t nestedIterations = 0;
};

struct ScalarKernel {
    std::vector<ScalarKernelInstruction> instructions;
    std::vector<std::string> slotNames;
    std::vector<TypedScalar> slots;
    std::vector<bool> initialized;
    std::vector<std::string> arraySlotNames;
    std::vector<TypedNumericArray> arrays;
    size_t loopSlot = 0;
    size_t registerCount = 0;
    size_t nestedLoopCount = 0;
};

} // namespace mparser
