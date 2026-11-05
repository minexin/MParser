#pragma once

#include "mparser/runtime/core/object_model/runtime_graphics.h"

namespace mparser {

enum class RuntimeGraphicsExportFormat { Png, Pdf };
enum class RuntimeGraphicsExportStatus { Written, UnsupportedFormat, DestinationRejected, Failed };

struct RuntimeGraphicsExportRequest {
    RuntimeGraphicsExportFormat format;
    std::string destination;
    RuntimeGraphicsId figureIdentity;
    std::string sceneJson;
};

// Internal synchronous adapter. The destination is untrusted UTF-8, not a
// permission to write. Written means the selected format was actually committed.
// The adapter owns authorization, encoding and atomic file publication; it must
// not reenter the invoking runtime or retain a reference to this request.
class RuntimeGraphicsExportAdapter {
public:
    virtual ~RuntimeGraphicsExportAdapter() = default;
    virtual RuntimeGraphicsExportStatus write(const RuntimeGraphicsExportRequest& request) const = 0;
};

} // namespace mparser
