#include "mparser/execution/jit/native_scalar_jit.h"
#include "mparser/runtime/core/value/runtime_shape.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
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

enum class NativeRuntimeStatus : int {
    Success = 0,
    InvalidColonRange = 1,
    InvalidLinearIndex = 2,
    LinearIndexOutOfBounds = 3,
    InvalidArraySlot = 4,
    ComplexResultRequired = 5,
};

struct NativeArrayView {
    double* elements = nullptr;
    size_t count = 0;
};

struct NativeContext {
    TypedScalar* slots = nullptr;
    TypedScalar* registers = nullptr;
    uint8_t* writtenSlots = nullptr;
    NativeArrayView* arrays = nullptr;
    size_t arrayCount = 0;
    uint8_t* writtenArrays = nullptr;
    const double* outerValues = nullptr;
    size_t outerValueCount = 0;
    size_t* loopIterations = nullptr;
    size_t* instructionCounts = nullptr;
    int status = 0;
};

static_assert(std::is_standard_layout_v<TypedScalar>);
static_assert(std::is_standard_layout_v<NativeArrayView>);
static_assert(std::is_standard_layout_v<NativeContext>);
static_assert(sizeof(size_t) == sizeof(sljit_sw));

using NativeEntry = void(SLJIT_FUNC*)(NativeContext*);

double SLJIT_FUNC nativePower(double left, double right) {
    return std::pow(left, right);
}

sljit_sw SLJIT_FUNC nativePowerRequiresComplex(double left,
                                                double right) {
    return left < 0.0 && std::isfinite(right) &&
                   std::floor(right) != right
               ? 1
               : 0;
}

std::optional<size_t> nativeLinearArrayOffset(
    NativeContext* context, sljit_sw arraySlot, double oneBasedIndex) {
    if (arraySlot < 0 ||
        static_cast<size_t>(arraySlot) >= context->arrayCount) {
        context->status =
            static_cast<int>(NativeRuntimeStatus::InvalidArraySlot);
        return std::nullopt;
    }
    const auto index =
        checkedRuntimeNonnegativeInteger(oneBasedIndex);
    if (!index || *index == 0) {
        context->status =
            static_cast<int>(NativeRuntimeStatus::InvalidLinearIndex);
        return std::nullopt;
    }
    if (*index > context->arrays[arraySlot].count) {
        context->status = static_cast<int>(
            NativeRuntimeStatus::LinearIndexOutOfBounds);
        return std::nullopt;
    }
    return *index - 1;
}

double SLJIT_FUNC nativeLoadArrayElement(
    NativeContext* context, sljit_sw arraySlot, double oneBasedIndex) {
    const auto offset = nativeLinearArrayOffset(
        context, arraySlot, oneBasedIndex);
    if (!offset) {
        return 0.0;
    }
    return context->arrays[arraySlot].elements[*offset];
}

void SLJIT_FUNC nativeStoreArrayElement(
    NativeContext* context, sljit_sw arraySlot, double oneBasedIndex,
    double value) {
    const auto offset = nativeLinearArrayOffset(
        context, arraySlot, oneBasedIndex);
    if (!offset) {
        return;
    }
    context->arrays[arraySlot].elements[*offset] = value;
    context->writtenArrays[arraySlot] = 1;
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
        : compiler_(compiler), kernel_(kernel),
          kernelLabels_(kernel.instructions.size() + 1, nullptr),
          pendingKernelJumps_(kernel.instructions.size() + 1),
          tracksInstructionCounts_(std::any_of(
              kernel.instructions.begin(), kernel.instructions.end(),
              [](const ScalarKernelInstruction& instruction) {
                  return instruction.op == ScalarKernelOp::Jump ||
                         instruction.op == ScalarKernelOp::JumpIfFalse;
              })) {}

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
        if (!bindKernelLabel(kernel_.instructions.size(), failureReason)) {
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
                SLJIT_IMM,
                static_cast<sljit_sw>(
                    NativeRuntimeStatus::InvalidColonRange));
            sljit_emit_return_void(compiler_);
        }
        if (!arrayFailureJumps_.empty()) {
            auto* arrayFailure = sljit_emit_label(compiler_);
            bindJumps(arrayFailureJumps_, arrayFailure);
            sljit_emit_return_void(compiler_);
        }
        if (!complexFailureJumps_.empty()) {
            auto* complexFailure = sljit_emit_label(compiler_);
            bindJumps(complexFailureJumps_, complexFailure);
            sljit_emit_op1(
                compiler_, SLJIT_MOV_S32,
                SLJIT_MEM1(SLJIT_S0),
                SLJIT_OFFSETOF(NativeContext, status),
                SLJIT_IMM,
                static_cast<sljit_sw>(
                    NativeRuntimeStatus::ComplexResultRequired));
            sljit_emit_return_void(compiler_);
        }
        for (const auto& jumps : pendingKernelJumps_) {
            if (!jumps.empty()) {
                failureReason =
                    "native scalar kernel has an unresolved branch target";
                return false;
            }
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

    bool bindKernelLabel(size_t pc, std::string& failureReason) {
        if (pc >= kernelLabels_.size()) {
            failureReason =
                "native scalar kernel label is outside the instruction stream";
            return false;
        }
        if (kernelLabels_[pc] != nullptr) {
            failureReason =
                "native scalar kernel instruction label was emitted twice";
            return false;
        }
        auto* label = sljit_emit_label(compiler_);
        if (label == nullptr) {
            failureReason = "SLJIT could not allocate a branch label";
            return false;
        }
        kernelLabels_[pc] = label;
        bindJumps(pendingKernelJumps_[pc], label);
        pendingKernelJumps_[pc].clear();
        return true;
    }

    bool bindKernelJump(sljit_jump* jump, size_t target,
                        std::string& failureReason) {
        if (jump == nullptr || target >= kernelLabels_.size()) {
            failureReason =
                "native scalar kernel could not emit a closed branch";
            return false;
        }
        if (kernelLabels_[target] != nullptr) {
            bindJump(jump, kernelLabels_[target]);
        } else {
            pendingKernelJumps_[target].push_back(jump);
        }
        return true;
    }

    void incrementInstructionCount(size_t pc) {
        if (!tracksInstructionCounts_) {
            return;
        }
        sljit_emit_op1(
            compiler_, SLJIT_MOV, SLJIT_R0, 0,
            SLJIT_MEM1(SLJIT_S0),
            SLJIT_OFFSETOF(NativeContext, instructionCounts));
        const auto offset = static_cast<sljit_sw>(pc * sizeof(size_t));
        sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R1, 0,
                       SLJIT_MEM1(SLJIT_R0), offset);
        sljit_emit_op2(compiler_, SLJIT_ADD, SLJIT_R1, 0,
                       SLJIT_R1, 0, SLJIT_IMM, 1);
        sljit_emit_op1(compiler_, SLJIT_MOV,
                       SLJIT_MEM1(SLJIT_R0), offset,
                       SLJIT_R1, 0);
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

    bool validateArraySlot(size_t arraySlot,
                           std::string& failureReason) const {
        if (arraySlot < kernel_.arrays.size()) {
            return true;
        }
        failureReason =
            "native scalar kernel has an invalid array slot";
        return false;
    }

    void appendArrayFailureJump() {
        sljit_emit_op1(
            compiler_, SLJIT_MOV_S32, SLJIT_R0, 0,
            SLJIT_MEM1(SLJIT_S0),
            SLJIT_OFFSETOF(NativeContext, status));
        arrayFailureJumps_.push_back(sljit_emit_cmp(
            compiler_, SLJIT_NOT_EQUAL, SLJIT_R0, 0,
            SLJIT_IMM, 0));
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
        if (instruction.op == ScalarKernelOp::LoadArrayElement) {
            if (!validateDestination(instruction.destination,
                                     failureReason) ||
                !validateOperand(instruction.left, failureReason) ||
                !validateArraySlot(instruction.arraySlot,
                                   failureReason)) {
                return false;
            }
            loadOperand(instruction.left, SLJIT_FR0);
            sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_S0, 0);
            sljit_emit_op1(
                compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
                static_cast<sljit_sw>(instruction.arraySlot));
            sljit_emit_icall(
                compiler_, SLJIT_CALL,
                SLJIT_ARGS3(F64, P, W, F64), SLJIT_IMM,
                SLJIT_FUNC_ADDR(nativeLoadArrayElement));
            appendArrayFailureJump();
            writeDestination(
                instruction.destination, SLJIT_FR0, SLJIT_IMM,
                static_cast<sljit_sw>(RuntimeNumericClass::Double));
            return true;
        }
        if (instruction.op == ScalarKernelOp::StoreArrayElement) {
            if (!validateOperand(instruction.left, failureReason) ||
                !validateOperand(instruction.right, failureReason) ||
                !validateArraySlot(instruction.arraySlot,
                                   failureReason)) {
                return false;
            }
            loadOperand(instruction.left, SLJIT_FR0);
            loadOperand(instruction.right, SLJIT_FR1);
            sljit_emit_op1(compiler_, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_S0, 0);
            sljit_emit_op1(
                compiler_, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
                static_cast<sljit_sw>(instruction.arraySlot));
            sljit_emit_icall(
                compiler_, SLJIT_CALL,
                SLJIT_ARGS4V(P, W, F64, F64), SLJIT_IMM,
                SLJIT_FUNC_ADDR(nativeStoreArrayElement));
            appendArrayFailureJump();
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
                SLJIT_ARGS2(W, F64, F64), SLJIT_IMM,
                SLJIT_FUNC_ADDR(nativePowerRequiresComplex));
            complexFailureJumps_.push_back(sljit_emit_cmp(
                compiler_, SLJIT_NOT_EQUAL, SLJIT_R0, 0,
                SLJIT_IMM, 0));
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
            if (instruction.op == ScalarKernelOp::ArcCosine ||
                instruction.op == ScalarKernelOp::ArcSine) {
                sljit_emit_fset64(compiler_, SLJIT_FR1, -1.0);
                complexFailureJumps_.push_back(sljit_emit_fcmp(
                    compiler_, SLJIT_ORDERED_LESS,
                    SLJIT_FR0, 0, SLJIT_FR1, 0));
                sljit_emit_fset64(compiler_, SLJIT_FR1, 1.0);
                complexFailureJumps_.push_back(sljit_emit_fcmp(
                    compiler_, SLJIT_ORDERED_GREATER,
                    SLJIT_FR0, 0, SLJIT_FR1, 0));
            } else if (instruction.op ==
                           ScalarKernelOp::Logarithm ||
                       instruction.op ==
                           ScalarKernelOp::SquareRoot) {
                sljit_emit_fset64(compiler_, SLJIT_FR1, 0.0);
                complexFailureJumps_.push_back(sljit_emit_fcmp(
                    compiler_, SLJIT_ORDERED_LESS,
                    SLJIT_FR0, 0, SLJIT_FR1, 0));
            }
            sljit_emit_icall(
                compiler_, SLJIT_CALL, SLJIT_ARGS1(F64, F64),
                SLJIT_IMM, unaryMathAddress(instruction.op));
            writeDouble();
            return true;
        case ScalarKernelOp::LoadArrayElement:
        case ScalarKernelOp::StoreArrayElement:
            failureReason =
                "native scalar kernel reached an unlowered array operation";
            return false;
        case ScalarKernelOp::Discard:
            return true;
        case ScalarKernelOp::Jump:
        case ScalarKernelOp::JumpIfFalse:
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
            if (!emitSpan(bodyBegin, bodyEnd, failureReason)) {
                return false;
            }
            return bindKernelLabel(bodyEnd, failureReason);
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
        if (!bindKernelLabel(bodyEnd, failureReason)) {
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
            if (!bindKernelLabel(pc, failureReason)) {
                return false;
            }
            if (instruction.op == ScalarKernelOp::LoopNext) {
                failureReason =
                    "native scalar kernel reached an unmatched loop latch";
                return false;
            }
            incrementInstructionCount(pc);
            if (instruction.op == ScalarKernelOp::Jump ||
                instruction.op == ScalarKernelOp::JumpIfFalse) {
                if (instruction.jumpTarget <= pc ||
                    instruction.jumpTarget > end) {
                    failureReason =
                        "native scalar kernel branch target escapes its control span";
                    return false;
                }

                sljit_jump* jump = nullptr;
                if (instruction.op == ScalarKernelOp::Jump) {
                    jump = sljit_emit_jump(compiler_, SLJIT_JUMP);
                } else {
                    if (!validateOperand(instruction.left,
                                         failureReason)) {
                        return false;
                    }
                    loadOperand(instruction.left, SLJIT_FR0);
                    sljit_emit_fset64(compiler_, SLJIT_FR1, 0.0);
                    jump = sljit_emit_fcmp(
                        compiler_, SLJIT_UNORDERED_OR_EQUAL,
                        SLJIT_FR0, 0, SLJIT_FR1, 0);
                }
                if (!bindKernelJump(jump, instruction.jumpTarget,
                                    failureReason)) {
                    return false;
                }
                ++pc;
                continue;
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
    std::vector<sljit_jump*> arrayFailureJumps_;
    std::vector<sljit_jump*> complexFailureJumps_;
    std::vector<sljit_label*> kernelLabels_;
    std::vector<std::vector<sljit_jump*>> pendingKernelJumps_;
    bool tracksInstructionCounts_ = false;
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

struct NativeCacheEvictionSummary {
    size_t count = 0;
    size_t codeBytes = 0;
};

struct NativeCacheStoreResult {
    std::shared_ptr<CompiledKernel> kernel;
    bool inserted = false;
    bool bypassed = false;
    bool duplicate = false;
    NativeCacheEvictionSummary evictions;
};

class NativeKernelCache {
public:
    std::shared_ptr<CompiledKernel> find(const std::string& key) {
        const std::lock_guard lock(mutex_);
        ++statistics_.lookupCount;
        const auto found = entries_.find(key);
        if (found == entries_.end()) {
            ++statistics_.missCount;
            return {};
        }

        ++statistics_.hitCount;
        touchLocked(found->second);
        return found->second.kernel;
    }

    NativeCacheStoreResult store(
        const std::string& key,
        std::shared_ptr<CompiledKernel> candidate) {
        NativeCacheStoreResult result;
        std::vector<std::shared_ptr<CompiledKernel>> released;
        {
            const std::lock_guard lock(mutex_);
            const auto existing = entries_.find(key);
            if (existing != entries_.end()) {
                touchLocked(existing->second);
                ++statistics_.duplicateCompilationCount;
                result.kernel = existing->second.kernel;
                result.duplicate = true;
                return result;
            }

            if (limits_.maxEntries == 0 ||
                limits_.maxCodeBytes == 0 ||
                candidate->codeSize > limits_.maxCodeBytes) {
                ++statistics_.bypassCount;
                result.kernel = std::move(candidate);
                result.bypassed = true;
                return result;
            }

            while (!entries_.empty() &&
                   (entries_.size() >= limits_.maxEntries ||
                    codeBytes_ >
                        limits_.maxCodeBytes - candidate->codeSize)) {
                evictLeastRecentLocked(released, result.evictions);
            }

            recency_.push_front(key);
            const auto [inserted, wasInserted] = entries_.emplace(
                key, Entry{candidate, candidate->codeSize,
                           recency_.begin()});
            if (!wasInserted) {
                recency_.pop_front();
                ++statistics_.duplicateCompilationCount;
                result.kernel = inserted->second.kernel;
                result.duplicate = true;
                return result;
            }
            codeBytes_ += candidate->codeSize;
            ++statistics_.insertionCount;
            result.kernel = std::move(candidate);
            result.inserted = true;
        }
        return result;
    }

    void recordCompilation(bool succeeded) {
        const std::lock_guard lock(mutex_);
        if (succeeded) {
            ++statistics_.compilationCount;
        } else {
            ++statistics_.compilationFailureCount;
        }
    }

    void configure(const NativeScalarJitCacheLimits& limits) {
        std::vector<std::shared_ptr<CompiledKernel>> released;
        NativeCacheEvictionSummary ignored;
        {
            const std::lock_guard lock(mutex_);
            limits_ = limits;
            while (!entries_.empty() &&
                   (entries_.size() > limits_.maxEntries ||
                    codeBytes_ > limits_.maxCodeBytes)) {
                evictLeastRecentLocked(released, ignored);
            }
        }
    }

    void clear() {
        std::vector<std::shared_ptr<CompiledKernel>> released;
        {
            const std::lock_guard lock(mutex_);
            ++statistics_.clearCount;
            statistics_.clearedEntryCount += entries_.size();
            statistics_.clearedCodeBytes += codeBytes_;
            released.reserve(entries_.size());
            for (auto& [key, entry] : entries_) {
                (void)key;
                released.push_back(std::move(entry.kernel));
            }
            entries_.clear();
            recency_.clear();
            codeBytes_ = 0;
        }
    }

    void resetStatistics() {
        const std::lock_guard lock(mutex_);
        statistics_ = {};
    }

    NativeScalarJitCacheStatistics statistics() const {
        const std::lock_guard lock(mutex_);
        auto result = statistics_;
        result.limits = limits_;
        result.entryCount = entries_.size();
        result.codeBytes = codeBytes_;
        return result;
    }

private:
    struct Entry {
        std::shared_ptr<CompiledKernel> kernel;
        size_t codeSize = 0;
        std::list<std::string>::iterator recency;
    };

    void touchLocked(Entry& entry) {
        if (entry.recency == recency_.begin()) {
            return;
        }
        recency_.splice(recency_.begin(), recency_, entry.recency);
        entry.recency = recency_.begin();
    }

    void evictLeastRecentLocked(
        std::vector<std::shared_ptr<CompiledKernel>>& released,
        NativeCacheEvictionSummary& summary) {
        const std::string key = recency_.back();
        const auto found = entries_.find(key);
        if (found == entries_.end()) {
            recency_.pop_back();
            return;
        }

        const size_t codeSize = found->second.codeSize;
        released.push_back(std::move(found->second.kernel));
        entries_.erase(found);
        recency_.pop_back();
        codeBytes_ -= codeSize;
        ++summary.count;
        summary.codeBytes += codeSize;
        ++statistics_.evictionCount;
        statistics_.evictedCodeBytes += codeSize;
    }

    mutable std::mutex mutex_;
    NativeScalarJitCacheLimits limits_;
    NativeScalarJitCacheStatistics statistics_;
    std::list<std::string> recency_;
    std::unordered_map<std::string, Entry> entries_;
    size_t codeBytes_ = 0;
};

NativeKernelCache& nativeKernelCache() {
    static NativeKernelCache cache;
    return cache;
}

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
    appendKeyValue(key, kernel.arrays.size());
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
        appendKeyValue(key, instruction.arraySlot);
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
    const std::vector<size_t>& loopIterations,
    const std::vector<size_t>& instructionCounts) {
    ScalarKernelExecutionCounters counters;
    counters.nestedIterations = std::accumulate(
        loopIterations.begin(), loopIterations.end(), size_t{0});

    std::vector<size_t> loopStack;
    for (size_t pc = 0; pc < kernel.instructions.size(); ++pc) {
        const auto& instruction = kernel.instructions[pc];
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

        const size_t multiplier = !instructionCounts.empty()
                                      ? instructionCounts[pc]
                                      : loopStack.empty()
                                            ? outerValueCount
                                            : loopIterations[
                                                  loopStack.back()];
        counters.sourceInstructions +=
            instruction.sourceInstructionCount * multiplier;
        counters.kernelInstructions += multiplier;
        if (instruction.op == ScalarKernelOp::LoopBegin) {
            loopStack.push_back(instruction.loopId);
        }
    }
    return counters;
}

} // namespace

bool nativeScalarJitAvailable() {
    return true;
}

std::string_view nativeScalarJitPlatform() {
    static const std::string platform = sljit_get_platform_name();
    return platform;
}

void configureNativeScalarJitCache(
    const NativeScalarJitCacheLimits& limits) {
    nativeKernelCache().configure(limits);
}

void clearNativeScalarJitCache() {
    nativeKernelCache().clear();
}

void resetNativeScalarJitCacheStatistics() {
    nativeKernelCache().resetStatistics();
}

NativeScalarJitCacheStatistics nativeScalarJitCacheStatistics() {
    return nativeKernelCache().statistics();
}

NativeScalarJitResult executeNativeScalarKernel(
    ScalarKernel& kernel, const double* outerValues,
    size_t outerValueCount) {
    NativeScalarJitResult result;
    if (kernel.arraySlotNames.size() != kernel.arrays.size()) {
        result.status = NativeScalarJitStatus::Unsupported;
        result.reason =
            "native scalar kernel array metadata is inconsistent";
        return result;
    }
    const auto key = kernelCacheKey(kernel);
    auto compiled = nativeKernelCache().find(key);
    result.cacheHit = static_cast<bool>(compiled);

    if (!compiled) {
        auto compilation = compileKernel(kernel);
        nativeKernelCache().recordCompilation(
            compilation.status == NativeScalarJitStatus::Executed);
        if (compilation.status != NativeScalarJitStatus::Executed) {
            result.status = compilation.status;
            result.reason = std::move(compilation.reason);
            return result;
        }
        compiled = std::move(compilation.kernel);
        result.compiled = true;
        auto stored = nativeKernelCache().store(key, compiled);
        compiled = std::move(stored.kernel);
        result.cacheStored = stored.inserted;
        result.cacheBypassed = stored.bypassed;
        result.cacheEvictionCount = stored.evictions.count;
        result.cacheEvictedCodeBytes = stored.evictions.codeBytes;
        if (stored.duplicate) {
            result.compiled = false;
            result.cacheHit = true;
        }
    }

    std::vector<TypedScalar> registers(kernel.registerCount);
    result.writtenSlots.assign(kernel.slots.size(), 0);
    result.writtenArrays.assign(kernel.arrays.size(), 0);
    std::vector<NativeArrayView> arrays;
    arrays.reserve(kernel.arrays.size());
    for (auto& array : kernel.arrays) {
        arrays.push_back(
            NativeArrayView{array.elements.data(),
                            array.elements.size()});
    }
    std::vector<size_t> loopIterations(kernel.nestedLoopCount, 0);
    const bool hasBranches = std::any_of(
        kernel.instructions.begin(), kernel.instructions.end(),
        [](const ScalarKernelInstruction& instruction) {
            return instruction.op == ScalarKernelOp::Jump ||
                   instruction.op == ScalarKernelOp::JumpIfFalse;
        });
    std::vector<size_t> instructionCounts(
        hasBranches ? kernel.instructions.size() : 0, 0);
    NativeContext context;
    context.slots = kernel.slots.data();
    context.registers = registers.data();
    context.writtenSlots = result.writtenSlots.data();
    context.arrays = arrays.data();
    context.arrayCount = arrays.size();
    context.writtenArrays = result.writtenArrays.data();
    context.outerValues = outerValues;
    context.outerValueCount = outerValueCount;
    context.loopIterations = loopIterations.data();
    context.instructionCounts = instructionCounts.data();
    compiled->entry()(&context);

    result.codeSize = compiled->codeSize;
    if (context.status != 0) {
        result.status = NativeScalarJitStatus::RuntimeFailed;
        switch (static_cast<NativeRuntimeStatus>(context.status)) {
        case NativeRuntimeStatus::InvalidColonRange:
            result.reason =
                "native typed nested colon range step cannot be zero";
            break;
        case NativeRuntimeStatus::InvalidLinearIndex:
            result.reason =
                "native typed linear index must be a finite positive integer";
            break;
        case NativeRuntimeStatus::LinearIndexOutOfBounds:
            result.reason =
                "native typed linear index exceeds the preallocated array bounds";
            break;
        case NativeRuntimeStatus::InvalidArraySlot:
            result.reason =
                "native typed kernel referenced an invalid array slot";
            break;
        case NativeRuntimeStatus::ComplexResultRequired:
            result.reason =
                "native typed operation requires a complex result";
            break;
        case NativeRuntimeStatus::Success:
            result.reason = "native typed kernel failed at runtime";
            break;
        }
        return result;
    }

    result.status = NativeScalarJitStatus::Executed;
    result.counters =
        deriveCounters(kernel, outerValueCount, loopIterations,
                       instructionCounts);
    const std::string kernelKind = kernel.arrays.empty()
                                       ? "scalar kernel"
                                       : "linear-array scalar kernel";
    if (result.cacheHit) {
        result.reason =
            "cached SLJIT native " + kernelKind + " executed";
    } else if (result.cacheBypassed) {
        result.reason = "compiled uncached SLJIT native " +
                        kernelKind + " executed";
    } else {
        result.reason =
            "compiled SLJIT native " + kernelKind + " executed";
    }
    return result;
}

#else

namespace {

std::mutex disabledCacheMutex;
NativeScalarJitCacheLimits disabledCacheLimits;
NativeScalarJitCacheStatistics disabledCacheStatistics;

} // namespace

bool nativeScalarJitAvailable() {
    return false;
}

std::string_view nativeScalarJitPlatform() {
    return "unavailable";
}

void configureNativeScalarJitCache(
    const NativeScalarJitCacheLimits& limits) {
    const std::lock_guard lock(disabledCacheMutex);
    disabledCacheLimits = limits;
}

void clearNativeScalarJitCache() {
    const std::lock_guard lock(disabledCacheMutex);
    ++disabledCacheStatistics.clearCount;
}

void resetNativeScalarJitCacheStatistics() {
    const std::lock_guard lock(disabledCacheMutex);
    disabledCacheStatistics = {};
}

NativeScalarJitCacheStatistics nativeScalarJitCacheStatistics() {
    const std::lock_guard lock(disabledCacheMutex);
    auto result = disabledCacheStatistics;
    result.limits = disabledCacheLimits;
    return result;
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
