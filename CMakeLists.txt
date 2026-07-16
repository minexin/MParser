cmake_minimum_required(VERSION 3.20)

project(MParser VERSION 0.55.0 LANGUAGES C CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MPARSER_ENABLE_NATIVE_JIT
    "Build the optional SLJIT native scalar-loop backend" ON)
set(MPARSER_SLJIT_SOURCE_DIR "" CACHE PATH
    "SLJIT source override; empty uses third_party/sljit")

if(MPARSER_ENABLE_NATIVE_JIT)
    if(MPARSER_SLJIT_SOURCE_DIR)
        set(_mparser_sljit_source_dir
            "${MPARSER_SLJIT_SOURCE_DIR}")
        message(STATUS
            "MParser native JIT: external SLJIT at "
            "${_mparser_sljit_source_dir}")
    else()
        set(_mparser_sljit_source_dir
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sljit")
        message(STATUS
            "MParser native JIT: bundled SLJIT revision "
            "3907e69005ba6e30b225000f24aaef3632f88347")
    endif()

    if(NOT EXISTS
       "${_mparser_sljit_source_dir}/sljit_src/sljitLir.c")
        message(FATAL_ERROR
            "SLJIT source tree is incomplete: "
            "${_mparser_sljit_source_dir}; expected sljit_src/sljitLir.c")
    endif()

    add_library(mparser_sljit STATIC
        "${_mparser_sljit_source_dir}/sljit_src/sljitLir.c")
    target_include_directories(mparser_sljit SYSTEM PUBLIC
        "${_mparser_sljit_source_dir}/sljit_src")
    if(MSVC)
        target_compile_options(mparser_sljit PRIVATE /W0)
    else()
        target_compile_options(mparser_sljit PRIVATE -w)
    endif()
endif()

add_library(mparser_core
    src/mparser/adaptive_module_runtime.cpp
    src/mparser/adaptive_bytecode_vm.cpp
    src/mparser/bytecode.cpp
    src/mparser/bytecode_dump.cpp
    src/mparser/bytecode_region.cpp
    src/mparser/bytecode_vm.cpp
    src/mparser/compiled_module.cpp
    src/mparser/function_signature.cpp
    src/mparser/interpreter.cpp
    src/mparser/lexer.cpp
    src/mparser/native_scalar_jit.cpp
    src/mparser/optimization_plan.cpp
    src/mparser/parser.cpp
    src/mparser/runtime_array_ops.cpp
    src/mparser/runtime_assignment.cpp
    src/mparser/runtime_benchmark.cpp
    src/mparser/runtime_index.cpp
    src/mparser/runtime_math.cpp
    src/mparser/runtime_numeric.cpp
    src/mparser/runtime_range.cpp
    src/mparser/runtime_reduction.cpp
    src/mparser/runtime_scan.cpp
    src/mparser/runtime_shape.cpp
    src/mparser/semantic.cpp
    src/mparser/semantic_dump.cpp
    src/mparser/source.cpp
    src/mparser/source_loader.cpp
    src/mparser/syntax.cpp
    src/mparser/syntax_dump.cpp
    src/mparser/typed_ir.cpp
    src/mparser/typed_region_executor.cpp
    src/mparser/token.cpp
)

target_include_directories(mparser_core PUBLIC src)
target_link_libraries(mparser_core PRIVATE Threads::Threads)

if(MPARSER_ENABLE_NATIVE_JIT)
    target_link_libraries(mparser_core PRIVATE mparser_sljit)
    target_compile_definitions(mparser_core PRIVATE MPARSER_HAS_SLJIT=1)
endif()

if(MSVC)
    target_compile_options(mparser_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(mparser_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(mparser src/main.cpp)
target_link_libraries(mparser PRIVATE mparser_core)
target_compile_definitions(mparser PRIVATE MPARSER_VERSION="${PROJECT_VERSION}")

enable_testing()

add_executable(mparser_smoke_tests tests/parser_smoke.cpp)
target_link_libraries(mparser_smoke_tests PRIVATE mparser_core)
add_test(NAME parser_smoke COMMAND mparser_smoke_tests)

add_executable(semantic_smoke_tests tests/semantic_smoke.cpp)
target_link_libraries(semantic_smoke_tests PRIVATE mparser_core)
add_test(NAME semantic_smoke COMMAND semantic_smoke_tests)

add_executable(bytecode_smoke_tests tests/bytecode_smoke.cpp)
target_link_libraries(bytecode_smoke_tests PRIVATE mparser_core)
add_test(NAME bytecode_smoke COMMAND bytecode_smoke_tests)

add_executable(bytecode_vm_smoke_tests tests/bytecode_vm_smoke.cpp)
target_link_libraries(bytecode_vm_smoke_tests PRIVATE mparser_core)
add_test(NAME bytecode_vm_smoke COMMAND bytecode_vm_smoke_tests)

add_executable(nd_array_smoke_tests tests/nd_array_smoke.cpp)
target_link_libraries(nd_array_smoke_tests PRIVATE mparser_core)
add_test(NAME nd_array_smoke COMMAND nd_array_smoke_tests)

add_executable(array_transform_smoke_tests
    tests/array_transform_smoke.cpp)
target_link_libraries(array_transform_smoke_tests PRIVATE mparser_core)
add_test(NAME array_transform_smoke COMMAND array_transform_smoke_tests)

add_executable(logical_index_smoke_tests
    tests/logical_index_smoke.cpp)
target_link_libraries(logical_index_smoke_tests PRIVATE mparser_core)
add_test(NAME logical_index_smoke COMMAND logical_index_smoke_tests)

add_executable(array_deletion_smoke_tests
    tests/array_deletion_smoke.cpp)
target_link_libraries(array_deletion_smoke_tests PRIVATE mparser_core)
add_test(NAME array_deletion_smoke COMMAND array_deletion_smoke_tests)

add_executable(reduction_find_smoke_tests
    tests/reduction_find_smoke.cpp)
target_link_libraries(reduction_find_smoke_tests PRIVATE mparser_core)
add_test(NAME reduction_find_smoke COMMAND reduction_find_smoke_tests)

add_executable(scan_diff_smoke_tests tests/scan_diff_smoke.cpp)
target_link_libraries(scan_diff_smoke_tests PRIVATE mparser_core)
add_test(NAME scan_diff_smoke COMMAND scan_diff_smoke_tests)

add_executable(optimization_plan_smoke_tests tests/optimization_plan_smoke.cpp)
target_link_libraries(optimization_plan_smoke_tests PRIVATE mparser_core)
add_test(NAME optimization_plan_smoke COMMAND optimization_plan_smoke_tests)

add_executable(bytecode_region_smoke_tests tests/bytecode_region_smoke.cpp)
target_link_libraries(bytecode_region_smoke_tests PRIVATE mparser_core)
add_test(NAME bytecode_region_smoke COMMAND bytecode_region_smoke_tests)

add_executable(typed_ir_smoke_tests tests/typed_ir_smoke.cpp)
target_link_libraries(typed_ir_smoke_tests PRIVATE mparser_core)
add_test(NAME typed_ir_smoke COMMAND typed_ir_smoke_tests)

add_executable(typed_region_executor_smoke_tests
    tests/typed_region_executor_smoke.cpp)
target_link_libraries(typed_region_executor_smoke_tests PRIVATE mparser_core)
add_test(NAME typed_region_executor_smoke
    COMMAND typed_region_executor_smoke_tests)

add_executable(native_scalar_jit_cache_smoke_tests
    tests/native_scalar_jit_cache_smoke.cpp)
target_link_libraries(native_scalar_jit_cache_smoke_tests PRIVATE
    mparser_core Threads::Threads)
add_test(NAME native_scalar_jit_cache_smoke
    COMMAND native_scalar_jit_cache_smoke_tests)

add_test(NAME scalar_kernel_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-typed-bytecode
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/typed_region_demo.m)
set_tests_properties(scalar_kernel_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "predecoded scalar kernel executed")

add_test(NAME guarded_jit_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-jit
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/nested_typed_loop_demo.m)
set_tests_properties(guarded_jit_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "predecoded nested scalar kernel executed.*baseline validation: skipped")

add_test(NAME nested_scalar_kernel_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-typed-bytecode
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/nested_typed_loop_demo.m)
set_tests_properties(nested_scalar_kernel_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "predecoded nested scalar kernel executed")

add_test(NAME nested_adaptive_kernel_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-adaptive-bytecode
        --adaptive-runs=3
        --adaptive-hot-loop=10
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/nested_typed_loop_demo.m)
set_tests_properties(nested_adaptive_kernel_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "predecoded nested scalar kernel executed")

add_test(NAME branched_scalar_kernel_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-typed-bytecode
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/branched_typed_loop_demo.m)
set_tests_properties(branched_scalar_kernel_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 23520")

add_test(NAME linear_array_kernel_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-typed-bytecode
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/typed_linear_array_demo.m)
set_tests_properties(linear_array_kernel_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "predecoded linear-array scalar kernel executed")

if(MPARSER_ENABLE_NATIVE_JIT)
    add_test(NAME native_scalar_kernel_cli_smoke
        COMMAND $<TARGET_FILE:mparser>
            --run-typed-bytecode
            --typed-backend=native
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/typed_region_demo.m)
    set_tests_properties(native_scalar_kernel_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "backend=native")

    add_test(NAME native_kernel_cache_cli_smoke
        COMMAND $<TARGET_FILE:mparser>
            --run-adaptive-bytecode
            --adaptive-runs=3
            --adaptive-hot-loop=10
            --typed-backend=native
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/nested_typed_loop_demo.m)
    set_tests_properties(native_kernel_cache_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "backend=native.*nativeCacheHits=1")

    add_test(NAME native_branched_kernel_cache_cli_smoke
        COMMAND $<TARGET_FILE:mparser>
            --run-adaptive-bytecode
            --adaptive-runs=3
            --adaptive-hot-loop=10
            --typed-backend=native
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/branched_typed_loop_demo.m)
    set_tests_properties(native_branched_kernel_cache_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "backend=native.*nativeCacheHits=1")

    add_test(NAME native_linear_array_kernel_cli_smoke
        COMMAND $<TARGET_FILE:mparser>
            --run-typed-bytecode
            --typed-backend=native
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/typed_linear_array_demo.m)
    set_tests_properties(native_linear_array_kernel_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "backend=native.*linear-array scalar kernel executed")

    add_test(NAME native_cache_lifecycle_cli_smoke
        COMMAND $<TARGET_FILE:mparser>
            --run-module-runtime
            --adaptive-hot-loop=5
            --typed-backend=native
            --native-cache-entries=2
            --native-cache-bytes=1048576
            --native-cache-stats
            --module-call=cache_add:2
            --module-call=cache_add:2
            --module-call=cache_add:2
            --module-call=cache_multiply:2
            --module-call=cache_multiply:2
            --module-call=cache_multiply:2
            --module-call=cache_absolute:2
            --module-call=cache_absolute:2
            --module-call=cache_absolute:2
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/native_cache_demo.m)
    set_tests_properties(native_cache_lifecycle_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "lifecycle: evictions=1")
else()
    add_test(NAME native_jit_disabled_cli_smoke
        COMMAND $<TARGET_FILE:mparser>
            --run-typed-bytecode
            --typed-backend=auto
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/typed_region_demo.m)
    set_tests_properties(native_jit_disabled_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "native scalar JIT support was disabled at build time")
endif()

add_executable(runtime_benchmark_smoke_tests
    tests/runtime_benchmark_smoke.cpp)
target_link_libraries(runtime_benchmark_smoke_tests PRIVATE mparser_core)
add_test(NAME runtime_benchmark_smoke COMMAND runtime_benchmark_smoke_tests)

add_executable(adaptive_bytecode_vm_smoke_tests
    tests/adaptive_bytecode_vm_smoke.cpp)
target_link_libraries(adaptive_bytecode_vm_smoke_tests PRIVATE mparser_core)
add_test(NAME adaptive_bytecode_vm_smoke
    COMMAND adaptive_bytecode_vm_smoke_tests)

add_executable(adaptive_module_runtime_smoke_tests
    tests/adaptive_module_runtime_smoke.cpp)
target_link_libraries(adaptive_module_runtime_smoke_tests PRIVATE mparser_core)
add_test(NAME adaptive_module_runtime_smoke
    COMMAND adaptive_module_runtime_smoke_tests)

add_executable(compiled_module_smoke_tests
    tests/compiled_module_smoke.cpp)
target_link_libraries(compiled_module_smoke_tests PRIVATE mparser_core)
add_test(NAME compiled_module_smoke COMMAND compiled_module_smoke_tests)

add_executable(source_loader_smoke_tests tests/source_loader_smoke.cpp)
target_link_libraries(source_loader_smoke_tests PRIVATE mparser_core)
add_test(NAME source_loader_smoke COMMAND source_loader_smoke_tests)

add_executable(namespace_function_smoke_tests
    tests/namespace_function_smoke.cpp)
target_link_libraries(namespace_function_smoke_tests PRIVATE mparser_core)
add_test(NAME namespace_function_smoke
    COMMAND namespace_function_smoke_tests)

add_executable(function_path_smoke_tests
    tests/function_path_smoke.cpp)
target_link_libraries(function_path_smoke_tests PRIVATE mparser_core)
add_test(NAME function_path_smoke
    COMMAND function_path_smoke_tests)

add_executable(class_folder_smoke_tests
    tests/class_folder_smoke.cpp)
target_link_libraries(class_folder_smoke_tests PRIVATE mparser_core)
add_test(NAME class_folder_smoke
    COMMAND class_folder_smoke_tests)

add_executable(class_private_function_smoke_tests
    tests/class_private_function_smoke.cpp)
target_link_libraries(class_private_function_smoke_tests PRIVATE mparser_core)
add_test(NAME class_private_function_smoke
    COMMAND class_private_function_smoke_tests)

add_test(NAME cross_file_class_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/cross_file_classes/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/cross_file_classes/run_demo.m)
set_tests_properties(cross_file_class_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "dynamic_code = 2")

add_test(NAME package_class_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/package_classes
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/package_classes/app/run_demo.m)
set_tests_properties(package_class_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "nested_code = 44")

add_test(NAME enumeration_class_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/app/run_demo.m)
set_tests_properties(enumeration_class_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 56")

add_test(NAME enumeration_class_module_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --module-info
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/app/run_demo.m)
set_tests_properties(enumeration_class_module_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "namespace=palette")

add_test(NAME enumeration_class_hir_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --hir
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/app/run_demo.m)
set_tests_properties(enumeration_class_hir_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "EnumerationMember Ready type=palette.Status")

add_test(NAME enumeration_class_query_loader_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/app/query_only.m)
set_tests_properties(enumeration_class_query_loader_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "query_count = 2")

add_test(NAME event_listener_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/app/run_demo.m)
set_tests_properties(event_listener_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 39")

add_test(NAME event_listener_module_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --module-info
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/app/run_demo.m)
set_tests_properties(event_listener_module_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "signals.onPulse")

add_test(NAME event_listener_hir_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --hir
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/app/run_demo.m)
set_tests_properties(event_listener_hir_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "Event Tick type=signals.Pulse")

add_test(NAME nd_array_bytecode_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/nd_arrays_demo.m)
set_tests_properties(nd_array_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 171")

add_test(NAME nd_array_interpreter_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/nd_arrays_demo.m)
set_tests_properties(nd_array_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 171")

add_test(NAME array_assignment_bytecode_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_assignment_demo.m)
set_tests_properties(array_assignment_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 860")

add_test(NAME array_assignment_interpreter_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_assignment_demo.m)
set_tests_properties(array_assignment_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 860")

add_test(NAME array_transform_bytecode_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_transform_demo.m)
set_tests_properties(array_transform_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2126")

add_test(NAME array_transform_interpreter_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_transform_demo.m)
set_tests_properties(array_transform_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2126")

add_test(NAME logical_index_bytecode_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/logical_index_demo.m)
set_tests_properties(logical_index_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 996")

add_test(NAME logical_index_interpreter_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/logical_index_demo.m)
set_tests_properties(logical_index_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 996")

add_test(NAME array_deletion_bytecode_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_deletion_demo.m)
set_tests_properties(array_deletion_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 252")

add_test(NAME array_deletion_interpreter_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_deletion_demo.m)
set_tests_properties(array_deletion_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 252")

add_test(NAME reduction_find_bytecode_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/reduction_find_demo.m)
set_tests_properties(reduction_find_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 1450")

add_test(NAME reduction_find_interpreter_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/reduction_find_demo.m)
set_tests_properties(reduction_find_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 1450")

add_test(NAME scan_diff_bytecode_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/scan_diff_demo.m)
set_tests_properties(scan_diff_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2359")

add_test(NAME scan_diff_interpreter_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/scan_diff_demo.m)
set_tests_properties(scan_diff_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2359")

add_test(NAME namespace_function_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/namespace_functions
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/namespace_functions/app/run_demo.m)
set_tests_properties(namespace_function_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "nested_value = 102")

add_test(NAME namespace_function_interpreter_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/namespace_functions
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/namespace_functions/app/run_function_demo.m)
set_tests_properties(namespace_function_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "nested_value = 102")

add_test(NAME function_path_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/first_library
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/second_library
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/global_library
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/app/run_demo.m)
set_tests_properties(function_path_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "outside_value = 101")

add_test(NAME function_path_interpreter_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/first_library
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/second_library
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/global_library
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/app/run_demo.m)
set_tests_properties(function_path_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "outside_value = 101")

add_test(NAME class_folder_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/app/run_demo.m)
set_tests_properties(class_folder_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "revealed = 107")

add_test(NAME class_folder_module_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --module-info
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/app/run_demo.m)
set_tests_properties(class_folder_module_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "method-of=folderpkg.Counter")

add_test(NAME class_private_function_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --run-bytecode
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/app/run_demo.m)
set_tests_properties(class_private_function_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "function_choice = 16")

add_test(NAME class_private_function_module_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --module-info
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/app/run_demo.m)
set_tests_properties(class_private_function_module_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "private-of=securepkg.Vault")

add_test(NAME class_private_function_hir_cli_smoke
    COMMAND $<TARGET_FILE:mparser>
        --hir
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/app/run_demo.m)
set_tests_properties(class_private_function_hir_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "lexical-class=securepkg.Vault")

add_test(NAME source_loader_diagnostic_cli_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMPARSER_EXECUTABLE=$<TARGET_FILE:mparser>
        -DENTRY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/source_loader_diagnostic/main.m
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_source_diagnostic.cmake)

add_executable(function_call_contract_smoke_tests
    tests/function_call_contract_smoke.cpp)
target_link_libraries(function_call_contract_smoke_tests PRIVATE mparser_core)
add_test(NAME function_call_contract_smoke
    COMMAND function_call_contract_smoke_tests)

add_executable(cell_varargs_smoke_tests tests/cell_varargs_smoke.cpp)
target_link_libraries(cell_varargs_smoke_tests PRIVATE mparser_core)
add_test(NAME cell_varargs_smoke COMMAND cell_varargs_smoke_tests)

add_executable(class_runtime_smoke_tests tests/class_runtime_smoke.cpp)
target_link_libraries(class_runtime_smoke_tests PRIVATE mparser_core)
add_test(NAME class_runtime_smoke COMMAND class_runtime_smoke_tests)

add_executable(enumeration_class_smoke_tests
    tests/enumeration_class_smoke.cpp)
target_link_libraries(enumeration_class_smoke_tests PRIVATE mparser_core)
add_test(NAME enumeration_class_smoke
    COMMAND enumeration_class_smoke_tests)

add_executable(event_listener_smoke_tests
    tests/event_listener_smoke.cpp)
target_link_libraries(event_listener_smoke_tests PRIVATE mparser_core)
add_test(NAME event_listener_smoke
    COMMAND event_listener_smoke_tests)

add_executable(handle_semantics_smoke_tests tests/handle_semantics_smoke.cpp)
target_link_libraries(handle_semantics_smoke_tests PRIVATE mparser_core)
add_test(NAME handle_semantics_smoke COMMAND handle_semantics_smoke_tests)

add_executable(class_inheritance_smoke_tests
    tests/class_inheritance_smoke.cpp)
target_link_libraries(class_inheritance_smoke_tests PRIVATE mparser_core)
add_test(NAME class_inheritance_smoke COMMAND class_inheritance_smoke_tests)

add_executable(superclass_construction_smoke_tests
    tests/superclass_construction_smoke.cpp)
target_link_libraries(superclass_construction_smoke_tests PRIVATE mparser_core)
add_test(NAME superclass_construction_smoke
    COMMAND superclass_construction_smoke_tests)

add_executable(property_validation_smoke_tests
    tests/property_validation_smoke.cpp)
target_link_libraries(property_validation_smoke_tests PRIVATE mparser_core)
add_test(NAME property_validation_smoke
    COMMAND property_validation_smoke_tests)

add_executable(class_member_access_smoke_tests
    tests/class_member_access_smoke.cpp)
target_link_libraries(class_member_access_smoke_tests PRIVATE mparser_core)
add_test(NAME class_member_access_smoke
    COMMAND class_member_access_smoke_tests)

add_executable(class_contract_smoke_tests tests/class_contract_smoke.cpp)
target_link_libraries(class_contract_smoke_tests PRIVATE mparser_core)
add_test(NAME class_contract_smoke COMMAND class_contract_smoke_tests)

add_executable(class_access_policy_smoke_tests
    tests/class_access_policy_smoke.cpp)
target_link_libraries(class_access_policy_smoke_tests PRIVATE mparser_core)
add_test(NAME class_access_policy_smoke
    COMMAND class_access_policy_smoke_tests)

add_executable(class_property_identity_smoke_tests
    tests/class_property_identity_smoke.cpp)
target_link_libraries(class_property_identity_smoke_tests PRIVATE mparser_core)
add_test(NAME class_property_identity_smoke
    COMMAND class_property_identity_smoke_tests)

add_executable(class_method_identity_smoke_tests
    tests/class_method_identity_smoke.cpp)
target_link_libraries(class_method_identity_smoke_tests PRIVATE mparser_core)
add_test(NAME class_method_identity_smoke
    COMMAND class_method_identity_smoke_tests)

add_executable(interpreter_smoke_tests tests/interpreter_smoke.cpp)
target_link_libraries(interpreter_smoke_tests PRIVATE mparser_core)
add_test(NAME interpreter_smoke COMMAND interpreter_smoke_tests)
