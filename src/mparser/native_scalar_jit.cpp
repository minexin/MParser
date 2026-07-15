#include "mparser/native_scalar_jit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#if defined(MPARSER_HAS_SLJIT)
#include <sljitLir.h>
#endif

namespace mparser {

#if defined(MPARSER_HAS_SLJIT)
namespace {

struct NativeContext {
    TypedScalar* slots = nullptr;
    TypedScalar* registers = nullptr;
    uint8_t* writtenSlots = nullptr;
    const double* outerValues = nullptr;
    size_t outerValueCount = 0;
    size_t* loopIterations = nullptr;
    int status = 0;
};

static_assert(std::is_standard_layout_v<TypedScalar>);
static_assert(std::is_standard_layout_v<NativeContext>);

using NativeEntry = void(SLJIT_FUNC*)(NativeContext*);

double SLJIT_FUNC nativePower(double left, double right) {
    return std::pow(left, right);
}

double SLJIT_FUNC nativeArcCosine(double value) {
    return std::acos(value);
}

double SLJIT_FUNC nativeArcSine(double value) {
    return std::asin(value);
}

double SLJIT_FUNC nativeArcTangent(double value) {
    return std::atan(value);
}

double SLJIT_FUNC nativeCosine(double value) {
    return std::cos(value);
}

double SLJIT_FUNC nativeExponential(double value) {
    return std::exp(value);
}

double SLJIT_FUNC nativeLogarithm(double value) {
    return std::log(value);
}

double SLJIT_FUNC nativeSine(double value) {
    return std::sin(value);
}

double SLJIT_FUNC nativeSquareRoot(double value) {
    return std::sqrt(value);
}

double SLJIT_FUNC nativeTangent(double value) {
    return std::tan(value);
}

sljit_sw unaryMathAddress(ScalarKernelOp operation) {
    switch (operation) {
    case ScalarKernelOp::ArcCosine:
        return SLJIT_FUNC_ADDR(nativeArcCosine);
    case ScalarKernelOp::ArcSine:
        return SLJIT_FUNC_ADDR(nativeArcSine);
    case ScalarKernelOp::ArcTangent:
        return SLJIT_FUNC_ADDR(nativeArcTangent);
    case ScalarKernelOp::Cosine:
        return SLJIT_FUNC_ADDR(nativeCosine);
    case ScalarKernelOp::Exponential:
        return SLJIT_FUNC_ADDR(nativeExponential);
    case ScalarKernelOp::Logarithm:
        return SLJIT_FUNC_ADDR(nativeLogarithm);
    case ScalarKernelOp::Sine:
        return SLJIT_FUNC_ADDR(nativeSine);
    case ScalarKernelOp::SquareRoot:
        return SLJIT_FUNC_ADDR(nativeSquareRoot);
    case ScalarKernelOp::Tangent:
        return SLJIT_FUNC_ADDR(nativeTangent);
    default:
        return 0;
    }
}

void bindJump(sljit_jump* jump, sljit_label* label) {
    if (jump != nullptr && label != nullptr) {
        sljit_set_label(jump, label);
    }
}

void bindJumps(const std::vector<sljit_jump*>& jumps,
               sljit_label* label) {
    for (auto* jump : jumps) {
        bindJump(jump, label);
    }
}

class NativeKernelEmitter {
public:
    NativeKernelEmitter(sljit_compiler* compiler,
                        const ScalarKernel& kernel)
        : compiler_(compiler), kernel_(kernel) {}

    bool emit(std::string& failureReason) {
        if (kernel_.nestedLoopCount >
            static_cast<size_t>(SLJIT_MAX_LOCAL_SIZE) /
                kLoopLocalSize) {
            failureReason =
                "native scalar kernel requires too much loop-local storage";
            return false;
        }

        const auto localSize = static_cast<sljit_s32>(
            kernel_.nestedLoopCount * kLoopLocalSize);
        sljit_emit_enter(
            compiler_, 0, SLJIT_ARGS1V(P),
            3 | SLJIT_ENTER_FLOAT(4), 3, localSize);

        sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_S1, 0,
                       SLJIT_IMM, 0);
        sljit_emit_op1(
            compiler_, SLJIT_MOV, SLJIT_S2, 0,
            SLJIT_MEM1(SLJIT_S0),
            SLJIT_OFFSETOF(NativeContext, outerValueCount));

        auto* loopLabel = sljit_emit_label(compiler_);
        auto* exitJump = sljit_emit_cmp(
            compiler_, SLJIT_GREATER_EQUAL, SLJIT_S1, 0,
            SLJIT_S2, 0);

        sljit_emit_op1(
            compiler_, SLJIT_MOV, SLJIT_R0, 0,
            SLJIT_MEM1(SLJIT_S0),
            SLJIT_OFFSETOF(NativeContext, outerValues));
        sljit_emit_fop1(compiler_, SLJIT_MOV_F64, SLJIT_FR0, 0,
                        SLJIT_MEM2(SLJIT_R0, SLJIT_S1), 3);
        writeDestination(
            ScalarKernelDestination{ScalarKernelStorage::Slot,
                                    kernel_.loopSlot},
            SLJIT_FR0, SLJIT_IMM,
            static_cast<sljit_sw>(RuntimeNumericClass::Double));

        if (!emitSpan(0, kernel_.instructions.size(), failureReason)) {
            return false;
        }

        sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_S1, 0,
                       SLJIT_S1, 0, SLJIT_IMM, 1);
        bindJump(sljit_emit_jump(compiler_, SLJIT_JUMP), loopLabel);

        auto* normalExit = sljit_emit_label(compiler_);
        bindJump(exitJump, normalExit);
        sljit_emit_return_void(compiler_);

        if (!rangeFailureJumps_.empty()) {
            auto* rangeFailure = sljit_emit_label(compiler_);
            bindJumps(rangeFailureJumps_, rangeFailure);
            sljit_emit_op1(
                compiler_, SLJIT_MOV_S32,
                SLJIT_MEM1(SLJIT_S0),
                SLJIT_OFFSETOF(NativeContext, status),
                SLJIT_IMM, 1);
            sljit_emit_return_void(compiler_);
        }
        return true;
    }

private:
    static constexpr sljit_sw kLoopLocalSize =
        static_cast<sljit_sw>(sizeof(double) * 3);

    sljit_sw loopLocalOffset(size_t loopId, size_t field) const {
        return static_cast<sljit_sw>(loopId * kLoopLocalSize +
                                     field * sizeof(double));
    }

    bool validateOperand(const ScalarKernelOperand& operand,
                         std::string& failureReason) const {
        if (operand.storage == ScalarKernelStorage::Slot &&
            operand.index >= kernel_.slots.size()) {
            failureReason = "native scalar kernel has an invalid slot operand";
            return false;
        }
        if (operand.storage == ScalarKernelStorage::Register &&
            operand.index >= kernel_.registerCount) {
            failureReason =
                "native scalar kernel has an invalid register operand";
            return false;
        }
        return true;
    }

    bool validateDestination(
        const ScalarKernelDestination& destination,
        std::string& failureReason) const {
        if (destination.storage == ScalarKernelStorage::Slot &&
            destination.index < kernel_.slots.size()) {
            return true;
        }
        if (destination.storage == ScalarKernelStorage::Register &&
            destination.index < kernel_.registerCount) {
            return true;
        }
        failureReason =
            "native scalar kernel has an invalid destination";
        return false;
    }

    void loadOperand(const ScalarKernelOperand& operand,
                     sljit_s32 floatRegister) {
        if (operand.storage == ScalarKernelStorage::Literal) {
            sljit_emit_fset64(compiler_, floatRegister,
                              operand.literal.value);
            return;
        }

        const sljit_sw contextOffset =
            operand.storage == ScalarKernelStorage::Slot
                ? SLJIT_OFFSETOF(NativeContext, slots)
                : SLJIT_OFFSETOF(NativeContext, registers);
        sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_S0), contextOffset);
        const auto itemOffset = static_cast<sljit_sw>(
            operand.index * sizeof(TypedScalar) +
            offsetof(TypedScalar, value));
        sljit_emit_fop1(compiler_, SLJIT_MOV_F64, floatRegister, 0,
                        SLJIT_MEM1(SLJIT_R0), itemOffset);
    }

    void loadNumericClass(const ScalarKernelOperand& operand,
                          sljit_s32 integerRegister) {
        if (operand.storage == ScalarKernelStorage::Literal) {
            sljit_emit_op1(
                compiler_, SLJIT_MOV_S32, integerRegister, 0,
                SLJIT_IMM,
                static_cast<sljit_sw>(operand.literal.numericClass));
            return;
        }

        const sljit_sw contextOffset =
            operand.storage == ScalarKernelStorage::Slot
                ? SLJIT_OFFSETOF(NativeContext, slots)
                : SLJIT_OFFSETOF(NativeContext, registers);
        sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_S0), contextOffset);
        const auto itemOffset = static_cast<sljit_sw>(
            operand.index * sizeof(TypedScalar) +
            offsetof(TypedScalar, numericClass));
        sljit_emit_op1(compiler_, SLJIT_MOV_S32, integerRegister, 0,
                       SLJIT_MEM1(SLJIT_R0), itemOffset);
    }

    void writeDestination(
        const ScalarKernelDestination& destination,
        sljit_s32 floatRegister, sljit_s32 classSource,
        sljit_sw classSourceValue) {
        const bool isSlot =
            destination.storage == ScalarKernelStorage::Slot;
        const sljit_sw contextOffset =
            isSlot ? SLJIT_OFFSETOF(NativeContext, slots)
                   : SLJIT_OFFSETOF(NativeContext, registers);
        sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_S0), contextOffset);
        const auto valueOffset = static_cast<sljit_sw>(
            destination.index * sizeof(TypedScalar) +
            offsetof(TypedScalar, value));
        const auto classOffset = static_cast<sljit_sw>(
            destination.index * sizeof(TypedScalar) +
            offsetof(TypedScalar, numericClass));
        sljit_emit_fop1(compiler_, SLJIT_MOV_F64,
                        SLJIT_MEM1(SLJIT_R0), valueOffset,
                        floatRegister, 0);
        sljit_emit_op1(compiler_, SLJIT_MOV_S32,
                       SLJIT_MEM1(SLJIT_R0), classOffset,
                       classSource, classSourceValue);

        if (isSlot) {
            sljit_emit_op1(
                compiler_, SLJIT_MOV, SLJIT_R2, 0,
                SLJIT_MEM1(SLJIT_S0),
                SLJIT_OFFSETOF(NativeContext, writtenSlots));
            sljit_emit_op1(
                compiler_, SLJIT_MOV_U8,
                SLJIT_MEM1(SLJIT_R2),
                static_cast<sljit_sw>(destination.index),
                SLJIT_IMM, 1);
        }
    }

    void emitBooleanComparison(sljit_s32 comparison,
                               const ScalarKernelOperand& left,
                               const ScalarKernelOperand& right) {
        loadOperand(left, SLJIT_FR0);
        loadOperand(right, SLJIT_FR1);
        auto* trueJump = sljit_emit_fcmp(
            compiler_, comparison, SLJIT_FR0, 0, SLJIT_FR1, 0);
        sljit_emit_fset64(compiler_, SLJIT_FR0, 0.0);
        auto* endJump = sljit_emit_jump(compiler_, SLJIT_JUMP);
        auto* trueLabel = sljit_emit_label(compiler_);
        bindJump(trueJump, trueLabel);
        sljit_emit_fset64(compiler_, SLJIT_FR0, 1.0);
        bindJump(endJump, sljit_emit_label(compiler_));
    }

    void emitLogicalNot(const ScalarKernelOperand& operand) {
        loadOperand(operand, SLJIT_FR0);
        sljit_emit_fset64(compiler_, SLJIT_FR1, 0.0);
        auto* trueJump = sljit_emit_fcmp(
            compiler_, SLJIT_UNORDERED_OR_EQUAL,
            SLJIT_FR0, 0, SLJIT_FR1, 0);
        sljit_emit_fset64(compiler_, SLJIT_FR0, 0.0);
        auto* endJump = sljit_emit_jump(compiler_, SLJIT_JUMP);
        bindJump(trueJump, sljit_emit_label(compiler_));
        sljit_emit_fset64(compiler_, SLJIT_FR0, 1.0);
        bindJump(endJump, sljit_emit_label(compiler_));
    }

    void emitLogicalAnd(const ScalarKernelOperand& left,
                        const ScalarKernelOperand& right) {
        sljit_emit_fset64(compiler_, SLJIT_FR2, 0.0);
        loadOperand(left, SLJIT_FR0);
        auto* falseLeft = sljit_emit_fcmp(
            compiler_, SLJIT_UNORDERED_OR_EQUAL,
            SLJIT_FR0, 0, SLJIT_FR2, 0);
        loadOperand(right, SLJIT_FR1);
        auto* falseRight = sljit_emit_fcmp(
            compiler_, SLJIT_UNORDERED_OR_EQUAL,
            SLJIT_FR1, 0, SLJIT_FR2, 0);
        sljit_emit_fset64(compiler_, SLJIT_FR0, 1.0);
        auto* endJump = sljit_emit_jump(compiler_, SLJIT_JUMP);
        auto* falseLabel = sljit_emit_label(compiler_);
        bindJump(falseLeft, falseLabel);
        bindJump(falseRight, falseLabel);
        sljit_emit_fset64(compiler_, SLJIT_FR0, 0.0);
        bindJump(endJump, sljit_emit_label(compiler_));
    }

    void emitLogicalOr(const ScalarKernelOperand& left,
                       const ScalarKernelOperand& right) {
        sljit_emit_fset64(compiler_, SLJIT_FR2, 0.0);
        loadOperand(left, SLJIT_FR0);
        auto* trueLeft = sljit_emit_fcmp(
            compiler_, SLJIT_ORDERED_NOT_EQUAL,
            SLJIT_FR0, 0, SLJIT_FR2, 0);
        loadOperand(right, SLJIT_FR1);
        auto* trueRight = sljit_emit_fcmp(
            compiler_, SLJIT_ORDERED_NOT_EQUAL,
            SLJIT_FR1, 0, SLJIT_FR2, 0);
        sljit_emit_fset64(compiler_, SLJIT_FR0, 0.0);
        auto* endJump = sljit_emit_jump(compiler_, SLJIT_JUMP);
        auto* trueLabel = sljit_emit_label(compiler_);
        bindJump(trueLeft, trueLabel);
        bindJump(trueRight, trueLabel);
        sljit_emit_fset64(compiler_, SLJIT_FR0, 1.0);
        bindJump(endJump, sljit_emit_label(compiler_));
    }

    bool emitInstruction(const ScalarKernelInstruction& instruction,
                         std::string& failureReason) {
        if (instruction.op == ScalarKernelOp::Discard) {
            return true;
        }
        if (!validateDestination(instruction.destination,
                                 failureReason) ||
            !validateOperand(instruction.left, failureReason)) {
            return false;
        }

        const auto writeDouble = [&]() {
            writeDestination(
                instruction.destination, SLJIT_FR0, SLJIT_IMM,
                static_cast<sljit_sw>(RuntimeNumericClass::Double));
        };
        const auto writeLogical = [&]() {
            writeDestination(
                instruction.destination, SLJIT_FR0, SLJIT_IMM,
                static_cast<sljit_sw>(RuntimeNumericClass::Logical));
        };

        switch (instruction.op) {
        case ScalarKernelOp::Copy:
            loadOperand(instruction.left, SLJIT_FR0);
            loadNumericClass(instruction.left, SLJIT_R1);
            writeDestination(instruction.destination, SLJIT_FR0,
                             SLJIT_R1, 0);
            return true;
        case ScalarKernelOp::UnaryPlus:
            loadOperand(instruction.left, SLJIT_FR0);
            writeDouble();
            return true;
        case ScalarKernelOp::UnaryMinus:
            loadOperand(instruction.left, SLJIT_FR0);
            sljit_emit_fop1(compiler_, SLJIT_NEG_F64, SLJIT_FR0, 0,
                            SLJIT_FR0, 0);
            writeDouble();
            return true;
        case ScalarKernelOp::LogicalNot:
            emitLogicalNot(instruction.left);
            writeLogical();
            return true;
        case ScalarKernelOp::Add:
        case ScalarKernelOp::Subtract:
        case ScalarKernelOp::Multiply:
        case ScalarKernelOp::Divide: {
            if (!validateOperand(instruction.right, failureReason)) {
                return false;
            }
            loadOperand(instruction.left, SLJIT_FR0);
            loadOperand(instruction.right, SLJIT_FR1);
            sljit_s32 operation = SLJIT_ADD_F64;
            if (instruction.op == ScalarKernelOp::Subtract) {
                operation = SLJIT_SUB_F64;
            } else if (instruction.op == ScalarKernelOp::Multiply) {
                operation = SLJIT_MUL_F64;
            } else if (instruction.op == ScalarKernelOp::Divide) {
                operation = SLJIT_DIV_F64;
            }
            sljit_emit_fop2(compiler_, operation, SLJIT_FR0, 0,
                            SLJIT_FR0, 0, SLJIT_FR1, 0);
            writeDouble();
            return true;
        }
        case ScalarKernelOp::Power:
            if (!validateOperand(instruction.right, failureReason)) {
                return false;
            }
            loadOperand(instruction.left, SLJIT_FR0);
            loadOperand(instruction.right, SLJIT_FR1);
            sljit_emit_icall(
                compiler_, SLJIT_CALL,
                SLJIT_ARGS2(F64, F64, F64), SLJIT_IMM,
                SLJIT_FUNC_ADDR(nativePower));
            writeDouble();
            return true;
        case ScalarKernelOp::Greater:
        case ScalarKernelOp::Less:
        case ScalarKernelOp::GreaterEqual:
        case ScalarKernelOp::LessEqual:
        case ScalarKernelOp::Equal:
        case ScalarKernelOp::NotEqual: {
            if (!validateOperand(instruction.right, failureReason)) {
                return false;
            }
            sljit_s32 comparison = SLJIT_ORDERED_GREATER;
            switch (instruction.op) {
            case ScalarKernelOp::Less:
                comparison = SLJIT_ORDERED_LESS;
                break;
            case ScalarKernelOp::GreaterEqual:
                comparison = SLJIT_ORDERED_GREATER_EQUAL;
                break;
            case ScalarKernelOp::LessEqual:
                comparison = SLJIT_ORDERED_LESS_EQUAL;
                break;
            case ScalarKernelOp::Equal:
                comparison = SLJIT_ORDERED_EQUAL;
                break;
            case ScalarKernelOp::NotEqual:
                comparison = SLJIT_UNORDERED_OR_NOT_EQUAL;
                break;
            default:
                break;
            }
            emitBooleanComparison(comparison, instruction.left,
                                  instruction.right);
            writeLogical();
            return true;
        }
        case ScalarKernelOp::LogicalAnd:
            if (!validateOperand(instruction.right, failureReason)) {
                return false;
            }
            emitLogicalAnd(instruction.left, instruction.right);
            writeLogical();
            return true;
        case ScalarKernelOp::LogicalOr:
            if (!validateOperand(instruction.right, failureReason)) {
                return false;
            }
            emitLogicalOr(instruction.left, instruction.right);
            writeLogical();
            return true;
        case ScalarKernelOp::Absolute:
            loadOperand(instruction.left, SLJIT_FR0);
            sljit_emit_fop1(compiler_, SLJIT_ABS_F64, SLJIT_FR0, 0,
                            SLJIT_FR0, 0);
            writeDouble();
            return true;
        case ScalarKernelOp::ArcCosine:
        case ScalarKernelOp::ArcSine:
        case ScalarKernelOp::ArcTangent:
        case ScalarKernelOp::Cosine:
        case ScalarKernelOp::Exponential:
        case ScalarKernelOp::Logarithm:
        case ScalarKernelOp::Sine:
        case ScalarKernelOp::SquareRoot:
        case ScalarKernelOp::Tangent:
            loadOperand(instruction.left, SLJIT_FR0);
            sljit_emit_icall(
                compiler_, SLJIT_CALL, SLJIT_ARGS1(F64, F64),
                SLJIT_IMM, unaryMathAddress(instruction.op));
            writeDouble();
            return true;
        case ScalarKernelOp::Discard:
            return true;
        case ScalarKernelOp::LoopBegin:
        case ScalarKernelOp::LoopNext:
            failureReason =
                "native scalar kernel emitted a control instruction as an operation";
            return false;
        }
        return false;
    }

    void incrementLoopCount(size_t loopId) {
        sljit_emit_op1(
            compiler_, SLJIT_MOV, SLJIT_R0, 0,
            SLJIT_MEM1(SLJIT_S0),
            SLJIT_OFFSETOF(NativeContext, loopIterations));
        const auto offset = static_cast<sljit_sw>(
            loopId * sizeof(size_t));
        sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0,
                       SLJIT_MEM1(SLJIT_R0), offset);
        sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0,
                       SLJIT_R1, 0, SLJIT_IMM, 1);
        sljit_emit_op1(compiler_, SLJIT_MOV,
                       SLJIT_MEM1(SLJIT_R0), offset,
                       SLJIT_R1, 0);
    }

    bool emitLoop(size_t pc, const ScalarKernelInstruction& loop,
                  std::string& failureReason) {
        if (loop.loopId >= kernel_.nestedLoopCount ||
            loop.jumpTarget <= pc + 1 ||
            loop.jumpTarget > kernel_.instructions.size() ||
            kernel_.instructions[loop.jumpTarget - 1].op !=
                ScalarKernelOp::LoopNext ||
            !validateDestination(loop.destination, failureReason) ||
            !validateOperand(loop.left, failureReason)) {
            if (failureReason.empty()) {
                failureReason =
                    "native scalar kernel has invalid loop boundaries";
            }
            return false;
        }

        const size_t bodyBegin = pc + 1;
        const size_t bodyEnd = loop.jumpTarget - 1;
        const auto currentOffset = loopLocalOffset(loop.loopId, 0);
        const auto stepOffset = loopLocalOffset(loop.loopId, 1);
        const auto stopOffset = loopLocalOffset(loop.loopId, 2);

        loadOperand(loop.left, SLJIT_FR0);
        sljit_emit_fop1(compiler_, SLJIT_MOV_F64,
                        SLJIT_MEM1(SLJIT_SP), currentOffset,
                        SLJIT_FR0, 0);

        if (loop.singleValueRange) {
            incrementLoopCount(loop.loopId);
            writeDestination(
                loop.destination, SLJIT_FR0, SLJIT_IMM,
                static_cast<sljit_sw>(RuntimeNumericClass::Double));
            return emitSpan(bodyBegin, bodyEnd, failureReason);
        }

        if (!validateOperand(loop.step, failureReason) ||
            !validateOperand(loop.right, failureReason)) {
            return false;
        }
        loadOperand(loop.step, SLJIT_FR1);
        loadOperand(loop.right, SLJIT_FR2);
        sljit_emit_fop1(compiler_, SLJIT_MOV_F64,
                        SLJIT_MEM1(SLJIT_SP), stepOffset,
                        SLJIT_FR1, 0);
        sljit_emit_fop1(compiler_, SLJIT_MOV_F64,
                        SLJIT_MEM1(SLJIT_SP), stopOffset,
                        SLJIT_FR2, 0);
        sljit_emit_fset64(compiler_, SLJIT_FR3, 0.0);
        rangeFailureJumps_.push_back(sljit_emit_fcmp(
            compiler_, SLJIT_ORDERED_EQUAL, SLJIT_FR1, 0,
            SLJIT_FR3, 0));

        auto* conditionLabel = sljit_emit_label(compiler_);
        sljit_emit_fop1(compiler_, SLJIT_MOV_F64, SLJIT_FR0, 0,
                        SLJIT_MEM1(SLJIT_SP), currentOffset);
        sljit_emit_fop1(compiler_, SLJIT_MOV_F64, SLJIT_FR1, 0,
                        SLJIT_MEM1(SLJIT_SP), stepOffset);
        sljit_emit_fop1(compiler_, SLJIT_MOV_F64, SLJIT_FR2, 0,
                        SLJIT_MEM1(SLJIT_SP), stopOffset);
        sljit_emit_fset64(compiler_, SLJIT_FR3, 0.0);

        std::vector<sljit_jump*> endJumps;
        endJumps.push_back(sljit_emit_fcmp(
            compiler_, SLJIT_UNORDERED, SLJIT_FR0, 0,
            SLJIT_FR2, 0));
        auto* positiveStep = sljit_emit_fcmp(
            compiler_, SLJIT_ORDERED_GREATER, SLJIT_FR1, 0,
            SLJIT_FR3, 0);
        endJumps.push_back(sljit_emit_fcmp(
            compiler_, SLJIT_ORDERED_LESS, SLJIT_FR0, 0,
            SLJIT_FR2, 0));
        auto* negativeBody = sljit_emit_jump(compiler_, SLJIT_JUMP);

        bindJump(positiveStep, sljit_emit_label(compiler_));
        endJumps.push_back(sljit_emit_fcmp(
            compiler_, SLJIT_ORDERED_GREATER, SLJIT_FR0, 0,
            SLJIT_FR2, 0));
        auto* bodyLabel = sljit_emit_label(compiler_);
        bindJump(negativeBody, bodyLabel);

        incrementLoopCount(loop.loopId);
        writeDestination(
            loop.destination, SLJIT_FR0, SLJIT_IMM,
            static_cast<sljit_sw>(RuntimeNumericClass::Double));
        if (!emitSpan(bodyBegin, bodyEnd, failureReason)) {
            return false;
        }

        sljit_emit_fop1(compiler_, SLJIT_MOV_F64, SLJIT_FR0, 0,
                        SLJIT_MEM1(SLJIT_SP), currentOffset);
        sljit_emit_fop1(compiler_, SLJIT_MOV_F64, SLJIT_FR1, 0,
                        SLJIT_MEM1(SLJIT_SP), stepOffset);
        sljit_emit_fop2(compiler_, SLJIT_ADD_F64, SLJIT_FR2, 0,
                        SLJIT_FR0, 0, SLJIT_FR1, 0);
        endJumps.push_back(sljit_emit_fcmp(
            compiler_, SLJIT_ORDERED_EQUAL, SLJIT_FR2, 0,
            SLJIT_FR0, 0));
        sljit_emit_fop1(compiler_, SLJIT_MOV_F64,
                        SLJIT_MEM1(SLJIT_SP), currentOffset,
                        SLJIT_FR2, 0);
        bindJump(sljit_emit_jump(compiler_, SLJIT_JUMP),
                 conditionLabel);
        bindJumps(endJumps, sljit_emit_label(compiler_));
        return true;
    }

    bool emitSpan(size_t begin, size_t end,
                  std::string& failureReason) {
        size_t pc = begin;
        while (pc < end) {
            const auto& instruction = kernel_.instructions[pc];
            if (instruction.op == ScalarKernelOp::LoopNext) {
                failureReason =
                    "native scalar kernel reached an unmatched loop latch";
                return false;
            }
            if (instruction.op == ScalarKernelOp::LoopBegin) {
                if (!emitLoop(pc, instruction, failureReason)) {
                    return false;
                }
                pc = instruction.jumpTarget;
                continue;
            }
            if (!emitInstruction(instruction, failureReason)) {
                return false;
            }
            ++pc;
        }
        return true;
    }

    sljit_compiler* compiler_ = nullptr;
    const ScalarKernel& kernel_;
    std::vector<sljit_jump*> rangeFailureJumps_;
};

struct CompiledKernel {
    void* code = nullptr;
    size_t codeSize = 0;

    ~CompiledKernel() {
        if (code != nullptr) {
            sljit_free_code(code, nullptr);
        }
    }

    NativeEntry entry() const {
        return reinterpret_cast<NativeEntry>(code);
    }
};

struct CompileResult {
    NativeScalarJitStatus status =
        NativeScalarJitStatus::CompilationFailed;
    std::shared_ptr<CompiledKernel> kernel;
    std::string reason;
};

template <typename Value>
void appendKeyValue(std::string& key, const Value& value) {
    static_assert(std::is_trivially_copyable_v<Value>);
    key.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void appendOperandKey(std::string& key,
                      const ScalarKernelOperand& operand) {
    appendKeyValue(key, operand.storage);
    appendKeyValue(key, operand.index);
    appendKeyValue(key, operand.literal.value);
    appendKeyValue(key, operand.literal.numericClass);
}

std::string kernelCacheKey(const ScalarKernel& kernel) {
    std::string key;
    key.reserve(kernel.instructions.size() * 128);
    appendKeyValue(key, kernel.slots.size());
    appendKeyValue(key, kernel.loopSlot);
    appendKeyValue(key, kernel.registerCount);
    appendKeyValue(key, kernel.nestedLoopCount);
    appendKeyValue(key, kernel.instructions.size());
    for (const auto& instruction : kernel.instructions) {
        appendKeyValue(key, instruction.op);
        appendKeyValue(key, instruction.destination.storage);
        appendKeyValue(key, instruction.destination.index);
        appendOperandKey(key, instruction.left);
        appendOperandKey(key, instruction.right);
        appendOperandKey(key, instruction.step);
        appendKeyValue(key, instruction.jumpTarget);
        appendKeyValue(key, instruction.loopId);
        appendKeyValue(key, instruction.singleValueRange);
    }
    return key;
}

CompileResult compileKernel(const ScalarKernel& kernel) {
    CompileResult result;
    auto* compiler = sljit_create_compiler(nullptr);
    if (compiler == nullptr) {
        result.reason = "SLJIT compiler allocation failed";
        return result;
    }

    std::string failureReason;
    NativeKernelEmitter emitter(compiler, kernel);
    if (!emitter.emit(failureReason)) {
        sljit_free_compiler(compiler);
        result.status = NativeScalarJitStatus::Unsupported;
        result.reason = std::move(failureReason);
        return result;
    }
    if (sljit_get_compiler_error(compiler) != SLJIT_SUCCESS) {
        sljit_free_compiler(compiler);
        result.reason = "SLJIT rejected the scalar kernel instruction stream";
        return result;
    }

    void* code = sljit_generate_code(compiler, 0, nullptr);
    const auto codeSize = static_cast<size_t>(
        sljit_get_generated_code_size(compiler));
    sljit_free_compiler(compiler);
    if (code == nullptr) {
        result.reason = "SLJIT executable memory generation failed";
        return result;
    }

    result.status = NativeScalarJitStatus::Executed;
    result.kernel = std::make_shared<CompiledKernel>();
    result.kernel->code = code;
    result.kernel->codeSize = codeSize;
    return result;
}

ScalarKernelExecutionCounters deriveCounters(
    const ScalarKernel& kernel, size_t outerValueCount,
    const std::vector<size_t>& loopIterations) {
    ScalarKernelExecutionCounters counters;
    counters.nestedIterations = std::accumulate(
        loopIterations.begin(), loopIterations.end(), size_t{0});

    std::vector<size_t> loopStack;
    for (const auto& instruction : kernel.instructions) {
        if (instruction.op == ScalarKernelOp::LoopNext) {
            const size_t multiplier =
                loopStack.empty() ? 0 : loopIterations[loopStack.back()];
            counters.sourceInstructions +=
                instruction.sourceInstructionCount * multiplier;
            if (!loopStack.empty()) {
                loopStack.pop_back();
            }
            continue;
        }

        const size_t multiplier =
            loopStack.empty() ? outerValueCount
                              : loopIterations[loopStack.back()];
        counters.sourceInstructions +=
            instruction.sourceInstructionCount * multiplier;
        counters.kernelInstructions += multiplier;
        if (instruction.op == ScalarKernelOp::LoopBegin) {
            loopStack.push_back(instruction.loopId);
        }
    }
    return counters;
}

std::mutex cacheMutex;
std::unordered_map<std::string, std::shared_ptr<CompiledKernel>>
    compiledKernels;

} // namespace

bool nativeScalarJitAvailable() {
    return true;
}

std::string_view nativeScalarJitPlatform() {
    static const std::string platform = sljit_get_platform_name();
    return platform;
}

NativeScalarJitResult executeNativeScalarKernel(
    ScalarKernel& kernel, const double* outerValues,
    size_t outerValueCount) {
    NativeScalarJitResult result;
    const auto key = kernelCacheKey(kernel);
    std::shared_ptr<CompiledKernel> compiled;
    {
        const std::lock_guard lock(cacheMutex);
        const auto cached = compiledKernels.find(key);
        if (cached != compiledKernels.end()) {
            compiled = cached->second;
            result.cacheHit = true;
        }
    }

    if (!compiled) {
        auto compilation = compileKernel(kernel);
        if (compilation.status != NativeScalarJitStatus::Executed) {
            result.status = compilation.status;
            result.reason = std::move(compilation.reason);
            return result;
        }
        compiled = std::move(compilation.kernel);
        result.compiled = true;
        const std::lock_guard lock(cacheMutex);
        const auto [iterator, inserted] =
            compiledKernels.emplace(key, compiled);
        if (!inserted) {
            compiled = iterator->second;
            result.compiled = false;
            result.cacheHit = true;
        }
    }

    std::vector<TypedScalar> registers(kernel.registerCount);
    result.writtenSlots.assign(kernel.slots.size(), 0);
    std::vector<size_t> loopIterations(kernel.nestedLoopCount, 0);
    NativeContext context;
    context.slots = kernel.slots.data();
    context.registers = registers.data();
    context.writtenSlots = result.writtenSlots.data();
    context.outerValues = outerValues;
    context.outerValueCount = outerValueCount;
    context.loopIterations = loopIterations.data();
    compiled->entry()(&context);

    result.codeSize = compiled->codeSize;
    if (context.status != 0) {
        result.status = NativeScalarJitStatus::RuntimeFailed;
        result.reason = "native typed nested colon range step cannot be zero";
        return result;
    }

    result.status = NativeScalarJitStatus::Executed;
    result.counters =
        deriveCounters(kernel, outerValueCount, loopIterations);
    result.reason = result.cacheHit
                        ? "cached SLJIT native scalar kernel executed"
                        : "compiled SLJIT native scalar kernel executed";
    return result;
}

#else

bool nativeScalarJitAvailable() {
    return false;
}

std::string_view nativeScalarJitPlatform() {
    return "unavailable";
}

NativeScalarJitResult executeNativeScalarKernel(
    ScalarKernel&, const double*, size_t) {
    NativeScalarJitResult result;
    result.status = NativeScalarJitStatus::Unavailable;
    result.reason = "native scalar JIT support was disabled at build time";
    return result;
}

#endif

} // namespace mparser
