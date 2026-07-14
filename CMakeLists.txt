cmake_minimum_required(VERSION 3.20)

project(MParser VERSION 0.36.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    src/mparser/optimization_plan.cpp
    src/mparser/parser.cpp
    src/mparser/runtime_benchmark.cpp
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
