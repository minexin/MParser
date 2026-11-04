#pragma once

#include "mparser/runtime/core/value/runtime_value.h"

#include <string>
#include <vector>

namespace mparser {

struct RuntimeColonSequence {
    double start = 0.0;
    double step = 1.0;
    double stop = 0.0;
    size_t count = 0;
    bool single = false;
};

struct RuntimeColonRange : RuntimeColonSequence {
    bool succeeded = false;
    std::string error;
};

struct RuntimeColonValueResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

RuntimeColonRange runtimePlanColonRange(
    const std::vector<double>& terms);
RuntimeColonRange runtimePlanColonRange(double start, double step,
                                        double stop);
double runtimeColonRangeElement(const RuntimeColonSequence& range, size_t index);
std::vector<double> runtimeMaterializeColonRange(
    const RuntimeColonRange& range);

RuntimeColonValueResult runtimeMaterializeColonValue(
    const std::vector<RuntimeValue>& operands);

} // namespace mparser
