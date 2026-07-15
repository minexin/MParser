#include "mparser/runtime_range.h"

#include <cmath>

namespace mparser {

RuntimeColonRange runtimePlanColonRange(double start, double step,
                                        double stop) {
    RuntimeColonRange range;
    range.start = start;
    range.step = step;
    range.stop = stop;
    if (step == 0.0) {
        range.error = "colon range step cannot be zero";
        return range;
    }
    range.succeeded = true;
    return range;
}

RuntimeColonRange runtimePlanColonRange(
    const std::vector<double>& terms) {
    if (terms.size() != 2 && terms.size() != 3) {
        RuntimeColonRange range;
        range.error = "colon range must have two or three operands";
        return range;
    }

    const double step = terms.size() == 3 ? terms[1] : 1.0;
    const double stop = terms.size() == 3 ? terms[2] : terms[1];
    return runtimePlanColonRange(terms[0], step, stop);
}

bool runtimeColonRangeContains(const RuntimeColonRange& range,
                               double value) {
    if (!range.succeeded || std::isnan(value) ||
        std::isnan(range.stop)) {
        return false;
    }
    return range.step > 0.0 ? value <= range.stop
                            : value >= range.stop;
}

std::vector<double> runtimeMaterializeColonRange(
    const RuntimeColonRange& range) {
    std::vector<double> values;
    for (double value = range.start;
         runtimeColonRangeContains(range, value);) {
        values.push_back(value);
        const double next = value + range.step;
        if (next == value) {
            break;
        }
        value = next;
    }
    return values;
}

} // namespace mparser
