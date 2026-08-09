#pragma once

#include "mparser/runtime_value.h"

#include <string>
#include <vector>

namespace mparser {

struct RuntimeColonRange {
    bool succeeded = false;
    double start = 0.0;
    double step = 1.0;
    double stop = 0.0;
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
bool runtimeColonRangeContains(const RuntimeColonRange& range,
                               double value);
std::vector<double> runtimeMaterializeColonRange(
    const RuntimeColonRange& range);

RuntimeColonValueResult runtimeMaterializeColonValue(
    const std::vector<RuntimeValue>& operands);

} // namespace mparser
