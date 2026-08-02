#include "performance_environment.h"

#include <algorithm>
#include <array>
#include <istream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser::performance {
namespace {

std::string trim(std::string value) {
    const auto notSpace = [](unsigned char character) {
        return character != ' ' && character != '\t' &&
               character != '\r' && character != '\n';
    };
    const auto begin = std::find_if(
        value.begin(), value.end(), notSpace);
    const auto end = std::find_if(
        value.rbegin(), value.rend(), notSpace)
                         .base();
    return begin < end ? std::string(begin, end) : std::string{};
}

} // namespace

std::string parseLinuxCpuModel(std::istream& input) {
    static constexpr std::array<std::string_view, 4> preferredKeys{
        "model name", "Model", "Hardware", "Processor"};
    static constexpr std::array<std::string_view, 5> armKeys{
        "CPU implementer", "CPU architecture", "CPU variant",
        "CPU part", "CPU revision"};
    std::array<std::string, armKeys.size()> armValues;

    std::string line;
    while (std::getline(input, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));
        if (value.empty()) {
            continue;
        }
        for (const auto candidate : preferredKeys) {
            if (key == candidate) {
                return value;
            }
        }
        for (std::size_t index = 0; index < armKeys.size(); ++index) {
            if (key == armKeys[index] && armValues[index].empty()) {
                armValues[index] = value;
                break;
            }
        }
    }

    std::vector<std::string> components;
    components.reserve(armKeys.size());
    for (std::size_t index = 0; index < armKeys.size(); ++index) {
        if (!armValues[index].empty()) {
            std::string name(armKeys[index]);
            name.erase(0, 4);
            components.push_back(
                std::move(name) + "=" + armValues[index]);
        }
    }
    if (components.empty()) {
        return "unknown";
    }

    std::string result = "ARM (";
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (index != 0) {
            result += ", ";
        }
        result += components[index];
    }
    result += ')';
    return result;
}

} // namespace mparser::performance
