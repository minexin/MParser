cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PROJECT_ROOT OR "${PROJECT_ROOT}" STREQUAL "")
    message(FATAL_ERROR "source layout validator requires PROJECT_ROOT")
endif()

set(core_root "${PROJECT_ROOT}/src/mparser/runtime/core")
set(owner_directories value indexing object_model session)
foreach(owner IN LISTS owner_directories)
    if(NOT IS_DIRECTORY "${core_root}/${owner}")
        message(FATAL_ERROR
            "runtime core owner directory is missing: ${owner}")
    endif()
endforeach()

foreach(required_directory IN ITEMS
        "${PROJECT_ROOT}/src/mparser/cli"
        "${PROJECT_ROOT}/src/mparser/runtime/builtins/datetime"
        "${PROJECT_ROOT}/src/mparser/execution/bytecode/vm")
    if(NOT IS_DIRECTORY "${required_directory}")
        message(FATAL_ERROR
            "required source ownership directory is missing: ${required_directory}")
    endif()
endforeach()

foreach(required_file IN ITEMS
        "${PROJECT_ROOT}/src/mparser/cli/main.cpp"
        "${PROJECT_ROOT}/src/mparser/execution/bytecode/vm/bytecode_vm.cpp"
        "${PROJECT_ROOT}/src/mparser/execution/bytecode/vm/bytecode_vm.h"
        "${PROJECT_ROOT}/src/mparser/execution/bytecode/vm/adaptive_bytecode_vm.cpp"
        "${PROJECT_ROOT}/src/mparser/execution/bytecode/vm/adaptive_bytecode_vm.h"
        "${PROJECT_ROOT}/src/mparser/runtime/builtins/datetime/runtime_datetime_builtins.cpp"
        "${PROJECT_ROOT}/src/mparser/runtime/builtins/datetime/runtime_datetime_builtins.h")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "required source ownership file is missing: ${required_file}")
    endif()
endforeach()

foreach(retired_source IN ITEMS
        "${PROJECT_ROOT}/src/main.cpp"
        "${PROJECT_ROOT}/src/mparser/execution/bytecode/bytecode_vm.cpp"
        "${PROJECT_ROOT}/src/mparser/execution/bytecode/adaptive_bytecode_vm.cpp")
    if(EXISTS "${retired_source}")
        message(FATAL_ERROR
            "source remains outside its owning directory: ${retired_source}")
    endif()
endforeach()

foreach(forwarding_header IN ITEMS
        "${PROJECT_ROOT}/src/mparser/execution/bytecode/bytecode_vm.h"
        "${PROJECT_ROOT}/src/mparser/execution/bytecode/adaptive_bytecode_vm.h")
    if(NOT EXISTS "${forwarding_header}")
        message(FATAL_ERROR
            "VM forwarding header is missing: ${forwarding_header}")
    endif()
endforeach()

set(allowed_root_files README.md runtime_output.h runtime_value.h)
file(GLOB root_entries LIST_DIRECTORIES TRUE RELATIVE "${core_root}"
    "${core_root}/*")
foreach(entry IN LISTS root_entries)
    if(IS_DIRECTORY "${core_root}/${entry}")
        list(FIND owner_directories "${entry}" owner_index)
        if(owner_index EQUAL -1)
            message(FATAL_ERROR
                "runtime core root contains an unowned directory: ${entry}")
        endif()
        continue()
    endif()
    list(FIND allowed_root_files "${entry}" allowed_index)
    if(allowed_index EQUAL -1)
        message(FATAL_ERROR
            "runtime core root contains an unowned file: ${entry}")
    endif()
endforeach()

set(pair_count 0)
foreach(owner IN LISTS owner_directories)
    file(GLOB implementations LIST_DIRECTORIES FALSE
        "${core_root}/${owner}/*.cpp")
    foreach(implementation IN LISTS implementations)
        get_filename_component(base "${implementation}" NAME_WE)
        if(NOT EXISTS "${core_root}/${owner}/${base}.h")
            message(FATAL_ERROR
                "runtime implementation has no owning header: "
                "${owner}/${base}.cpp")
        endif()
        math(EXPR pair_count "${pair_count} + 1")
    endforeach()
endforeach()
if(pair_count LESS 24)
    message(FATAL_ERROR
        "runtime core layout lost expected implementation pairs: ${pair_count}")
endif()

file(GLOB_RECURSE core_sources LIST_DIRECTORIES FALSE
    "${core_root}/*.h" "${core_root}/*.cpp")
foreach(source IN LISTS core_sources)
    file(READ "${source}" contents)
    if(contents MATCHES
            "#[ \t]*include[ \t]*\"mparser/runtime/builtins/")
        file(RELATIVE_PATH relative "${PROJECT_ROOT}" "${source}")
        message(FATAL_ERROR
            "runtime core depends on a builtin-family header: ${relative}")
    endif()
endforeach()

set(facade_runtime_value
    "mparser/runtime/core/value/runtime_value.h")
set(facade_runtime_output
    "mparser/runtime/core/session/runtime_output.h")
foreach(facade IN ITEMS runtime_value runtime_output)
    file(READ "${core_root}/${facade}.h" facade_contents)
    set(expected_include "${facade_${facade}}")
    string(FIND "${facade_contents}"
        "#include \"${expected_include}\"" include_index)
    if(include_index EQUAL -1)
        message(FATAL_ERROR
            "runtime core facade ${facade}.h does not forward to its owner")
    endif()
    if(facade_contents MATCHES
            "(namespace[ \t]+mparser|class[ \t]+|struct[ \t]+)")
        message(FATAL_ERROR
            "runtime core facade ${facade}.h contains declarations")
    endif()
endforeach()

foreach(retired IN ITEMS
        src/mparser/execution/jit/runtime_native_numeric.h
        src/mparser/execution/jit/runtime_native_numeric.cpp)
    if(EXISTS "${PROJECT_ROOT}/${retired}")
        message(FATAL_ERROR
            "generic dense numeric backend remains JIT-owned: ${retired}")
    endif()
endforeach()

message(STATUS
    "MParser source layout validated: ${pair_count} runtime pairs, "
    "four owners, two facades, and no core-to-builtin dependency; "
    "CLI, builtin-family, and VM ownership boundaries are present")
