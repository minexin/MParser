#include "mparser/runtime_mat_builtins.h"

#include "mparser/filesystem_utf8.h"
#include "mparser/runtime_mat_file.h"
#include "mparser/runtime_struct.h"
#include "mparser/runtime_system.h"
#include "mparser/runtime_text.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mparser {
namespace {

BuiltinResult failure(const BuiltinCall &call, std::string message,
                      std::string identifier) {
  return BuiltinResult::failure(call.span, std::move(message),
                                std::move(identifier));
}

RuntimeWorkspace *currentWorkspace(const BuiltinCall &call) {
  return call.context && call.context->workspace
             ? call.context->workspace->variables
             : nullptr;
}

RuntimeSystemContext *systemContext(const BuiltinCall &call) {
  return call.context ? call.context->systemContext : nullptr;
}

std::optional<std::string> textArgument(const RuntimeValue &value) {
  return runtimeTextScalarUtf8(value);
}

std::filesystem::path matFilePath(std::string_view text) {
  auto path = pathFromUtf8(text);
  if (!path.has_extension()) {
    path += ".mat";
  }
  return path;
}

RuntimeSystemResult<std::filesystem::path>
readableMatPath(RuntimeSystemContext &context,
                const std::filesystem::path &requested) {
  auto exists = context.regularFileExists(requested);
  if (!exists.succeeded) {
    return RuntimeSystemResult<std::filesystem::path>::failure(
        std::move(exists.error));
  }
  if (exists.value) {
    return RuntimeSystemResult<std::filesystem::path>::success(requested);
  }
  if (requested.is_absolute() || requested.has_parent_path() ||
      !context.hasCapability(RuntimeSystemCapability::SearchPaths)) {
    return RuntimeSystemResult<std::filesystem::path>::failure(
        "MAT file does not exist: " + pathToNativeUtf8(requested));
  }
  const auto paths = context.searchPaths();
  if (!paths.succeeded) {
    return RuntimeSystemResult<std::filesystem::path>::failure(paths.error);
  }
  for (const auto &path : paths.value) {
    const auto candidate = path / requested;
    exists = context.regularFileExists(candidate);
    if (!exists.succeeded) {
      return RuntimeSystemResult<std::filesystem::path>::failure(
          std::move(exists.error));
    }
    if (exists.value) {
      return RuntimeSystemResult<std::filesystem::path>::success(candidate);
    }
  }
  return RuntimeSystemResult<std::filesystem::path>::failure(
      "MAT file does not exist: " + pathToNativeUtf8(requested));
}

RuntimeSystemResult<std::string>
readBinaryFile(RuntimeSystemContext &context,
               const std::filesystem::path &path) {
  RuntimeFileOpenOptions options;
  options.readable = true;
  options.binary = true;
  options.permission = "rb";
  auto opened = context.openFile(path, options);
  if (!opened.succeeded) {
    return RuntimeSystemResult<std::string>::failure(std::move(opened.error));
  }
  auto bytes = context.readFileRemaining(opened.value);
  const auto closed = context.closeFile(opened.value);
  if (!bytes.succeeded) {
    return RuntimeSystemResult<std::string>::failure(std::move(bytes.error));
  }
  if (!closed.succeeded) {
    return RuntimeSystemResult<std::string>::failure(closed.error);
  }
  return bytes;
}

RuntimeSystemStatus writeBinaryFile(RuntimeSystemContext &context,
                                    const std::filesystem::path &path,
                                    std::string_view bytes) {
  RuntimeFileOpenOptions options;
  options.writable = true;
  options.truncate = true;
  options.binary = true;
  options.permission = "wb";
  auto opened = context.openFile(path, options);
  if (!opened.succeeded) {
    return RuntimeSystemStatus::failure(std::move(opened.error));
  }
  auto written = context.writeFile(opened.value, bytes);
  const auto closed = context.closeFile(opened.value);
  if (!written.succeeded) {
    return RuntimeSystemStatus::failure(std::move(written.error));
  }
  if (written.value != bytes.size()) {
    return RuntimeSystemStatus::failure(
        "MAT file write completed only partially");
  }
  return closed;
}

struct SaveRequest {
  std::filesystem::path path = matFilePath("matlab.mat");
  std::vector<std::string> variables;
  bool compress = true;
};

RuntimeSystemResult<SaveRequest>
parseSaveRequest(const std::vector<RuntimeValue> &arguments) {
  SaveRequest request;
  size_t index = 0;
  if (!arguments.empty()) {
    const auto first = textArgument(arguments.front());
    if (!first || first->find('\0') != std::string::npos) {
      return RuntimeSystemResult<SaveRequest>::failure(
          "save arguments must be text scalars without null bytes");
    }
    if (!first->starts_with('-')) {
      if (first->empty()) {
        return RuntimeSystemResult<SaveRequest>::failure(
            "save file name cannot be empty");
      }
      request.path = matFilePath(*first);
      index = 1;
    }
  }

  std::set<std::string, std::less<>> seen;
  for (; index < arguments.size(); ++index) {
    const auto text = textArgument(arguments[index]);
    if (!text || text->find('\0') != std::string::npos) {
      return RuntimeSystemResult<SaveRequest>::failure(
          "save arguments must be text scalars without null bytes");
    }
    if (text->starts_with('-')) {
      if (*text == "-v7" || *text == "-mat") {
        continue;
      }
      if (*text == "-nocompression") {
        request.compress = false;
        continue;
      }
      return RuntimeSystemResult<SaveRequest>::failure(
          "unsupported save option: " + *text);
    }
    if (!isRuntimeStructFieldName(*text)) {
      return RuntimeSystemResult<SaveRequest>::failure(
          "save variable name is invalid: " + *text);
    }
    if (seen.insert(*text).second) {
      request.variables.push_back(*text);
    }
  }
  return RuntimeSystemResult<SaveRequest>::success(std::move(request));
}

BuiltinResult saveBuiltin(const BuiltinCall &call) {
  RuntimeWorkspace *workspace = currentWorkspace(call);
  RuntimeSystemContext *context = systemContext(call);
  if (!workspace || !context) {
    return failure(call, "save requires workspace and system services",
                   "MParser:MissingBuiltinContext");
  }
  const auto request = parseSaveRequest(call.arguments);
  if (!request.succeeded) {
    const bool option = request.error.starts_with("unsupported save");
    return failure(call, request.error,
                   option ? "MParser:UnsupportedMatFileOption"
                          : "MParser:InvalidMatFileCall");
  }

  std::vector<RuntimeMatVariable> variables;
  if (request.value.variables.empty()) {
    variables.reserve(workspace->size());
    for (const auto &[name, value] : *workspace) {
      variables.push_back({name, value});
    }
  } else {
    variables.reserve(request.value.variables.size());
    for (const std::string &name : request.value.variables) {
      const auto found = workspace->find(name);
      if (found == workspace->end()) {
        return failure(call, "save variable does not exist: " + name,
                       "MParser:WorkspaceVariableNotFound");
      }
      variables.push_back({name, found->second});
    }
  }

  RuntimeMatEncodeOptions options;
  options.compress = request.value.compress;
  const auto encoded = runtimeEncodeMatV5(variables, options);
  if (!encoded.succeeded) {
    return failure(call, encoded.error, "MParser:MatFileEncodeFailed");
  }
  const auto written =
      writeBinaryFile(*context, request.value.path, encoded.bytes);
  return written.succeeded
             ? BuiltinResult::success()
             : failure(call, written.error, "MParser:MatFileWriteFailed");
}

struct LoadRequest {
  std::filesystem::path path = matFilePath("matlab.mat");
  std::vector<std::string> variables;
};

RuntimeSystemResult<LoadRequest>
parseLoadRequest(const std::vector<RuntimeValue> &arguments) {
  LoadRequest request;
  size_t index = 0;
  if (!arguments.empty()) {
    const auto first = textArgument(arguments.front());
    if (!first || first->find('\0') != std::string::npos) {
      return RuntimeSystemResult<LoadRequest>::failure(
          "load arguments must be text scalars without null bytes");
    }
    if (!first->starts_with('-')) {
      if (first->empty()) {
        return RuntimeSystemResult<LoadRequest>::failure(
            "load file name cannot be empty");
      }
      request.path = matFilePath(*first);
      index = 1;
    }
  }

  std::set<std::string, std::less<>> seen;
  for (; index < arguments.size(); ++index) {
    const auto text = textArgument(arguments[index]);
    if (!text || text->find('\0') != std::string::npos) {
      return RuntimeSystemResult<LoadRequest>::failure(
          "load arguments must be text scalars without null bytes");
    }
    if (text->starts_with('-')) {
      if (*text == "-mat") {
        continue;
      }
      return RuntimeSystemResult<LoadRequest>::failure(
          "unsupported load option: " + *text);
    }
    if (!isRuntimeStructFieldName(*text)) {
      return RuntimeSystemResult<LoadRequest>::failure(
          "load variable name is invalid: " + *text);
    }
    if (seen.insert(*text).second) {
      request.variables.push_back(*text);
    }
  }
  return RuntimeSystemResult<LoadRequest>::success(std::move(request));
}

BuiltinResult loadBuiltin(const BuiltinCall &call) {
  RuntimeWorkspace *workspace = currentWorkspace(call);
  RuntimeSystemContext *context = systemContext(call);
  if (!workspace || !context) {
    return failure(call, "load requires workspace and system services",
                   "MParser:MissingBuiltinContext");
  }
  const auto request = parseLoadRequest(call.arguments);
  if (!request.succeeded) {
    const bool option = request.error.starts_with("unsupported load");
    return failure(call, request.error,
                   option ? "MParser:UnsupportedMatFileOption"
                          : "MParser:InvalidMatFileCall");
  }

  const auto path = readableMatPath(*context, request.value.path);
  if (!path.succeeded) {
    const bool missing = path.error.starts_with("MAT file does not exist");
    return failure(call, path.error,
                   missing ? "MParser:MatFileNotFound"
                           : "MParser:MatFileReadFailed");
  }
  const auto bytes = readBinaryFile(*context, path.value);
  if (!bytes.succeeded) {
    return failure(call, bytes.error, "MParser:MatFileReadFailed");
  }
  auto decoded = runtimeDecodeMatV5(bytes.value);
  if (!decoded.succeeded) {
    return failure(call, decoded.error, "MParser:MatFileDecodeFailed");
  }

  const std::set<std::string, std::less<>> selected(
      request.value.variables.begin(), request.value.variables.end());
  std::vector<std::string> fieldOrder;
  RuntimeWorkspace loaded;
  for (RuntimeMatVariable &variable : decoded.variables) {
    if (!selected.empty() && !selected.contains(variable.name)) {
      continue;
    }
    if (!loaded.contains(variable.name)) {
      fieldOrder.push_back(variable.name);
    }
    loaded[variable.name] = std::move(variable.value);
  }

  if (call.requestedOutputCount == 1) {
    return BuiltinResult::success({makeRuntimeStructArrayValue(
        std::move(fieldOrder), {std::move(loaded)}, {1, 1})});
  }

  RuntimeWorkspace updated = *workspace;
  for (auto &[name, value] : loaded) {
    updated[name] = std::move(value);
  }
  workspace->swap(updated);
  return BuiltinResult::success();
}

} // namespace

bool isRuntimeMatBuiltin(std::string_view name) {
  return name == "load" || name == "save";
}

BuiltinResult invokeRuntimeMatBuiltin(std::string_view name,
                                      const BuiltinCall &call) {
  if (name == "save") {
    return saveBuiltin(call);
  }
  if (name == "load") {
    return loadBuiltin(call);
  }
  return failure(call, "unknown MAT-file builtin: " + std::string(name),
                 "MParser:UnknownBuiltin");
}

} // namespace mparser
