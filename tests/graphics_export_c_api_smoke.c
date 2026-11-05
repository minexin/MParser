#include "mparser/c_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "graphics export C API check failed at line %d\n", __LINE__); exit(1); \
} } while (0)

typedef struct renderer_state {
    const mparser_module* module;
    mparser_runtime* runtime;
    mparser_session* session;
    mparser_graphics_export_status status;
    unsigned calls;
} renderer_state;

static int equals(mparser_utf8_view view, const char* expected) {
    return view.size == strlen(expected) && memcmp(view.data, expected, view.size) == 0;
}

static mparser_graphics_export_status render(void* data, const mparser_graphics_export_request* request) {
    renderer_state* state = (renderer_state*)data;
    mparser_result* reentry = NULL;
    REQUIRE(state && request && request->struct_size == sizeof(*request));
    REQUIRE(request->abi_generation == 2 && request->snapshot_version == 4);
    REQUIRE(request->figure_identity != 0 && request->format == MPARSER_GRAPHICS_EXPORT_PNG);
    REQUIRE(equals(request->destination, "demo.png") && request->scene_json.size > 128);
    REQUIRE(mparser_module_execute(state->module, NULL, &reentry) == MPARSER_API_STATUS_INVALID_ARGUMENT && reentry == NULL);
    REQUIRE(mparser_runtime_execute(state->runtime, state->module, NULL, &reentry) == MPARSER_API_STATUS_INVALID_ARGUMENT && reentry == NULL);
    REQUIRE(mparser_session_execute(state->session, NULL, &reentry) == MPARSER_API_STATUS_INVALID_ARGUMENT && reentry == NULL);
    REQUIRE(mparser_runtime_reset(state->runtime) == MPARSER_API_STATUS_INVALID_ARGUMENT);
    REQUIRE(mparser_session_reset(state->session) == MPARSER_API_STATUS_INVALID_ARGUMENT);
    ++state->calls;
    return state->status;
}

static void failed(mparser_result* result, const char* identifier) {
    const mparser_diagnostic* diagnostic = mparser_result_diagnostic(result, 0);
    REQUIRE(result && !mparser_result_succeeded(result) && diagnostic);
    REQUIRE(equals(mparser_diagnostic_identifier(diagnostic), identifier));
    mparser_result_release(result);
}

static mparser_debug_action resume(void* data, const mparser_debug_event* event) {
    REQUIRE(data && event);
    ++*(unsigned*)data;
    return MPARSER_DEBUG_CONTINUE;
}

int main(void) {
    const char source[] = "plot(1:3); print('demo','-dpng');";
    mparser_module* module = NULL;
    mparser_runtime* runtime = NULL;
    mparser_session* session = NULL;
    mparser_result* result = NULL;
    mparser_invocation_options options;
    REQUIRE(mparser_module_compile_utf8(source, sizeof(source) - 1, "export.m", 8, &module) == MPARSER_API_STATUS_OK);
    REQUIRE(mparser_runtime_create(NULL, &runtime) == MPARSER_API_STATUS_OK);
    REQUIRE(mparser_module_create_session(module, &session) == MPARSER_API_STATUS_OK);
    REQUIRE(MPARSER_INVOCATION_OPTIONS_INIT(&options) == MPARSER_API_STATUS_OK);
    REQUIRE(!options.graphics_export && !options.graphics_export_user_data);
    renderer_state renderer = {module, runtime, session, MPARSER_GRAPHICS_EXPORT_WRITTEN, 0};
    options.graphics_export = render;
    options.graphics_export_user_data = &renderer;

    /* Allocate real short buffers so ASan catches any tail over-read. */
    const size_t prefix_sizes[] = {MPARSER_INVOCATION_OPTIONS_SIZE,
        offsetof(mparser_invocation_options, graphics_export), offsetof(mparser_invocation_options, graphics_export_user_data)};
    for (size_t i = 0; i < sizeof(prefix_sizes) / sizeof(prefix_sizes[0]); ++i) {
        const size_t size = prefix_sizes[i];
        mparser_invocation_options* old = (mparser_invocation_options*)malloc(size);
        REQUIRE(old != NULL);
        memcpy(old, &options, size);
        old->struct_size = (uint32_t)size;
        REQUIRE(mparser_module_execute(module, old, &result) == MPARSER_API_STATUS_OK);
        failed(result, "MParser:GraphicsRendererUnavailable");
        free(old);
    }
    REQUIRE(renderer.calls == 0);

    mparser_debugger* debugger = NULL;
    unsigned pauses = 0;
    REQUIRE(mparser_debugger_create(resume, &pauses, &debugger) == MPARSER_API_STATUS_OK);
    REQUIRE(mparser_debugger_request_pause(debugger) == MPARSER_API_STATUS_OK);
    options.debugger = debugger;
    REQUIRE(mparser_module_execute(module, &options, &result) == MPARSER_API_STATUS_OK);
    REQUIRE(mparser_result_succeeded(result) && renderer.calls == 1 && pauses == 1);
    mparser_result_release(result);
    options.debugger = NULL;
    mparser_debugger_release(debugger);
    REQUIRE(mparser_session_execute(session, &options, &result) == MPARSER_API_STATUS_OK);
    REQUIRE(mparser_result_succeeded(result) && renderer.calls == 2);
    mparser_result_release(result);
    renderer.status = MPARSER_GRAPHICS_EXPORT_DESTINATION_REJECTED;
    REQUIRE(mparser_runtime_execute(runtime, module, &options, &result) == MPARSER_API_STATUS_OK);
    failed(result, "MParser:GraphicsExportDestinationRejected");
    REQUIRE(renderer.calls == 3);
    renderer.status = MPARSER_GRAPHICS_EXPORT_UNSUPPORTED_FORMAT;
    REQUIRE(mparser_runtime_execute(runtime, module, &options, &result) == MPARSER_API_STATUS_OK);
    failed(result, "MParser:UnsupportedExportFormat");
    renderer.status = UINT32_MAX;
    REQUIRE(mparser_runtime_execute(runtime, module, &options, &result) == MPARSER_API_STATUS_OK);
    failed(result, "MParser:GraphicsExportFailed");
    REQUIRE(renderer.calls == 5 && mparser_runtime_reset(runtime) == MPARSER_API_STATUS_OK);
    REQUIRE(mparser_runtime_execute(runtime, module, NULL, &result) == MPARSER_API_STATUS_OK);
    failed(result, "MParser:GraphicsRendererUnavailable");
    REQUIRE(renderer.calls == 5);
    mparser_session_release(session);
    mparser_runtime_release(runtime);
    mparser_module_release(module);
    puts("PASS C renderer callback, three execute paths, truncated tails, reentry and typed failures (no encoding)");
    return 0;
}
