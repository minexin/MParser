#pragma once

#include <iosfwd>
#include <string>

namespace mparser::performance {

std::string parseLinuxCpuModel(std::istream& input);

} // namespace mparser::performance
