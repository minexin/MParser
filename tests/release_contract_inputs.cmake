include_guard(GLOBAL)

set(mparser_active_sdk_contract_path
    docs/host-integration-contract-development.json)
set(mparser_release_contract_paths
    CMakeLists.txt
    CMakePresets.json
    .github/workflows/ci.yml
    docs/public-contract-v1.3.json
    docs/public-contract-v1.14.json
    docs/public-contract-v1.2.json
    docs/public-contract-v1.json
    docs/cli-contract-v1.json
    docs/machine-result-v1.schema.json
    docs/compatibility-matrix.json
    docs/release-process.md
    docs/release-authentication.md
    tests/validate_release_authentication_input.cmake
    ${mparser_active_sdk_contract_path}
    tests/release_contract_inputs.cmake)

function(mparser_require_canonical_release_file
         root git revision relative_path working_file)
    execute_process(
        COMMAND "${git}" -C "${root}"
            rev-parse --verify "${revision}:${relative_path}"
        OUTPUT_VARIABLE committed_blob
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND "${git}" -C "${root}"
            hash-object --no-filters -- "${working_file}"
        OUTPUT_VARIABLE working_blob
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT working_blob STREQUAL committed_blob)
        message(FATAL_ERROR
            "Non-canonical publication input: ${relative_path}. "
            "Use a fresh checkout of ${revision}; git status alone can hide "
            "line-ending differences.")
    endif()
endfunction()

function(mparser_require_canonical_release_contracts root git revision)
    # The active manifest also binds SDK members not listed as direct inputs.
    set(paths ${mparser_release_contract_paths})
    file(READ "${root}/${mparser_active_sdk_contract_path}" sdk_contract)
    string(JSON count LENGTH "${sdk_contract}" artifacts)
    if(count LESS 1)
        message(FATAL_ERROR "The active SDK contract has no artifact bindings")
    endif()
    math(EXPR last "${count} - 1")
    foreach(index RANGE 0 ${last})
        string(JSON path GET "${sdk_contract}" artifacts ${index} path)
        list(APPEND paths "${path}")
    endforeach()
    list(REMOVE_DUPLICATES paths)
    foreach(path IN LISTS paths)
        mparser_require_canonical_release_file(
            "${root}" "${git}" "${revision}" "${path}" "${root}/${path}")
    endforeach()
    foreach(index RANGE 0 ${last})
        string(JSON path GET "${sdk_contract}" artifacts ${index} path)
        string(JSON declared GET "${sdk_contract}" artifacts ${index} sha256_lf)
        file(SHA256 "${root}/${path}" actual)
        if(NOT actual STREQUAL declared)
            message(FATAL_ERROR "Active SDK contract digest differs: ${path}")
        endif()
    endforeach()
endfunction()
