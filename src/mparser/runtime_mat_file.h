#pragma once

#include "mparser/runtime_value.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct RuntimeMatVariable {
  std::string name;
  RuntimeValue value;
};

struct RuntimeMatEncodeOptions {
  bool compress = true;
  size_t maximumBytes = 256U * 1024U * 1024U;
  size_t maximumDepth = 64;
};

struct RuntimeMatDecodeOptions {
  size_t maximumBytes = 256U * 1024U * 1024U;
  size_t maximumDepth = 64;
};

struct RuntimeMatEncodeResult {
  bool succeeded = false;
  std::string bytes;
  std::string error;
};

struct RuntimeMatDecodeResult {
  bool succeeded = false;
  std::vector<RuntimeMatVariable> variables;
  std::string error;
};

RuntimeMatEncodeResult
runtimeEncodeMatV5(const std::vector<RuntimeMatVariable> &variables,
                   const RuntimeMatEncodeOptions &options = {});

RuntimeMatDecodeResult
runtimeDecodeMatV5(std::string_view bytes,
                   const RuntimeMatDecodeOptions &options = {});

} // namespace mparser
