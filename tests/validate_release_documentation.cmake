cmake_minimum_required(VERSION 3.20)

foreach(required IN ITEMS
        PROJECT_ROOT
        CLI_CONTRACT
        MPARSER_EXECUTABLE
        ENTRY_SOURCE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR
            "release documentation validator requires ${required}")
    endif()
endforeach()

set(required_documents
    docs/README.md
    docs/user-manual.md
    docs/build-and-install.md
    docs/support-matrix.md
    docs/cli-reference.md
    docs/jit-and-fallback.md
    docs/runtime-boundaries.md
    docs/migration-v1.0.md
    docs/release-process.md
    docs/release-authentication.md
    docs/release-evidence/v0.90.1-authentication/README.md
    docs/release-notes-v1.0.md
    docs/roadmap-v1.x.md
    docs/v0.90.1.md
    docs/v1.0-cross-platform-validation.md
    docs/v1.0-jit-scope-decision.md
    docs/v1.0-documentation.md
    docs/embedding-c-api.md
    docs/embedding-cpp-api.md
    docs/c-abi-compatibility.md
    docs/machine-result-protocol.md
    docs/extending-builtins.md
    docs/versioning-and-deprecation.md)

foreach(relative_path IN LISTS required_documents)
    set(document_path "${PROJECT_ROOT}/${relative_path}")
    if(NOT EXISTS "${document_path}")
        message(FATAL_ERROR
            "required release document is missing: ${relative_path}")
    endif()
    file(READ "${document_path}" document_text)
    string(LENGTH "${document_text}" document_length)
    if(document_length LESS 200)
        message(FATAL_ERROR
            "release document is unexpectedly short: ${relative_path}")
    endif()
    if(document_text MATCHES "(^|[^A-Za-z])(TODO|TBD)([^A-Za-z]|$)")
        message(FATAL_ERROR
            "release document retains a TODO/TBD marker: ${relative_path}")
    endif()
endforeach()

file(READ "${PROJECT_ROOT}/docs/README.md" documentation_index)
file(READ "${PROJECT_ROOT}/docs/user-manual.md" user_manual)
file(READ "${PROJECT_ROOT}/docs/build-and-install.md" build_guide)
file(READ "${PROJECT_ROOT}/docs/support-matrix.md" support_matrix)
file(READ "${PROJECT_ROOT}/docs/cli-reference.md" cli_reference)
file(READ "${PROJECT_ROOT}/docs/jit-and-fallback.md" jit_guide)
file(READ "${PROJECT_ROOT}/docs/runtime-boundaries.md" runtime_guide)
file(READ "${PROJECT_ROOT}/docs/migration-v1.0.md" migration_guide)
file(READ "${PROJECT_ROOT}/docs/release-process.md" release_process)
file(READ "${PROJECT_ROOT}/docs/release-authentication.md"
    release_authentication)
file(READ
    "${PROJECT_ROOT}/docs/release-evidence/v0.90.1-authentication/manifest.json"
    release_authentication_evidence)
file(READ "${PROJECT_ROOT}/docs/embedding-c-api.md" embedding_c_api)
file(READ "${PROJECT_ROOT}/docs/architecture.md" architecture)
file(READ "${PROJECT_ROOT}/docs/compatibility-matrix.json"
    compatibility_matrix)
file(READ "${PROJECT_ROOT}/docs/v1.0-performance-baseline.md"
    performance_guide)
file(READ "${PROJECT_ROOT}/docs/v1.0-cross-platform-validation.md"
    cross_platform_validation)
file(READ "${PROJECT_ROOT}/README.md" project_readme)
file(READ "${CLI_CONTRACT}" cli_contract)
file(READ "${PROJECT_ROOT}/.github/workflows/ci.yml" ci_workflow)
file(READ "${PROJECT_ROOT}/CMakePresets.json" cmake_presets)

function(require_text variable needle description)
    string(FIND "${${variable}}" "${needle}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR
            "${description} is missing required text: ${needle}")
    endif()
endfunction()

function(reject_text variable needle description)
    string(FIND "${${variable}}" "${needle}" found_at)
    if(NOT found_at EQUAL -1)
        message(FATAL_ERROR
            "${description} retains forbidden text: ${needle}")
    endif()
endfunction()

foreach(required_ci_path IN ITEMS
        "$PWD/build-sdk/install-sdk"
        "$PWD/build-sdk/relocated-sdk"
        "build-sdk/macos-sdk-version.txt"
        "$PWD/build-arm64-jit/install-sdk"
        "build-arm64-jit/arm64-jit-sdk-version.txt"
        "$PWD/build-arm64-portable/install-sdk"
        "build-arm64-portable/arm64-portable-sdk-version.txt"
        "build-arm64-portable/arm64-branch-output.txt")
    require_text(ci_workflow "${required_ci_path}"
        "release CI build-tree artifact policy")
endforeach()

foreach(forbidden_ci_path IN ITEMS
        "$PWD/install-sdk"
        "$PWD/relocated-sdk"
        "mv install-sdk relocated-sdk"
        "$PWD/install-arm64-jit"
        "./install-arm64-jit/bin/mparser"
        "$PWD/install-arm64-portable"
        "./install-arm64-portable/bin/mparser"
        "tee macos-sdk-version.txt"
        "tee arm64-jit-sdk-version.txt"
        "tee arm64-portable-sdk-version.txt"
        "tee arm64-branch-output.txt")
    reject_text(ci_workflow "${forbidden_ci_path}"
        "release CI source-tree cleanliness policy")
endforeach()

foreach(required_performance_ci_text IN ITEMS
        "MPARSER_PERFORMANCE_EVIDENCE_REVISION="
        "mparser_performance_evidence"
        "build-ci/performance-evidence/*.json"
        "mparser-performance-0.90.1-windows-x86_64"
        "mparser-performance-0.90.1-linux-x86_64"
        "mparser-performance-0.90.1-macos-"
        "actions/upload-artifact@v7")
    require_text(ci_workflow "${required_performance_ci_text}"
        "native performance evidence CI policy")
endforeach()
reject_text(ci_workflow "actions/upload-artifact@v4"
    "Node 24 artifact upload policy")

foreach(required_authentication_ci_text IN ITEMS
        "authenticate_release:"
        "default: false"
        "release-authentication:"
        "github.event_name == 'workflow_dispatch' && inputs.authenticate_release"
        "Require the exact release tag"
        "refs/tags/v*"
        "actions/download-artifact@v5"
        "tests/validate_release_authentication_input.cmake"
        "MPARSER_REQUIRE_CLEAN_SOURCE=ON"
        "MPARSER_REQUIRE_EXACT_PACKAGE_SET=ON"
        "Expected one $archive_name; found"
        "sigstore/cosign-installer@6f9f17788090df1f26f669e9d70d6ae9567deba6"
        "cosign sign-blob"
        "cosign verify-blob"
        "https://token.actions.githubusercontent.com"
        "actions/upload-artifact@v7")
    require_text(ci_workflow "${required_authentication_ci_text}"
        "release authentication CI policy")
endforeach()
foreach(expected_archive IN ITEMS
        "mparser-0.90.1-windows-x86_64.zip"
        "mparser-0.90.1-linux-x86_64.tar.gz"
        "mparser-0.90.1-linux-aarch64.tar.gz"
        "mparser-0.90.1-macos-x86_64.tar.gz"
        "mparser-0.90.1-macos-arm64.tar.gz")
    require_text(ci_workflow "${expected_archive}"
        "release authentication platform set")
endforeach()
foreach(forbidden_release_upload IN ITEMS
        "build-ci/packages/*"
        "build-arm64-jit/packages/*"
        "_CPack_Packages")
    reject_text(ci_workflow "${forbidden_release_upload}"
        "release artifact top-level file boundary")
endforeach()
foreach(required_release_upload IN ITEMS
        "build-ci/packages/mparser-0.90.1-windows-x86_64.zip.sha256"
        "build-ci/packages/mparser-0.90.1-windows-x86_64.zip.provenance.json"
        "build-ci/packages/mparser-0.90.1-linux-x86_64.tar.gz.sha256"
        "build-ci/packages/mparser-0.90.1-linux-x86_64.tar.gz.provenance.json"
        "build-ci/packages/mparser-0.90.1-macos-\${{ matrix.arch }}.tar.gz.sha256"
        "build-ci/packages/mparser-0.90.1-macos-\${{ matrix.arch }}.tar.gz.provenance.json"
        "build-arm64-jit/packages/mparser-0.90.1-linux-aarch64.tar.gz.sha256"
        "build-arm64-jit/packages/mparser-0.90.1-linux-aarch64.tar.gz.provenance.json"
        "build-ci/packages/SHA256SUMS"
        "build-arm64-jit/packages/SHA256SUMS")
    require_text(ci_workflow "${required_release_upload}"
        "release artifact explicit top-level file set")
endforeach()
string(REGEX MATCHALL "id-token: write"
    authentication_token_matches "${ci_workflow}")
list(LENGTH authentication_token_matches authentication_token_count)
if(NOT authentication_token_count EQUAL 1)
    message(FATAL_ERROR
        "only the release-authentication job may request an OIDC token")
endif()
reject_text(ci_workflow "attestations: write"
    "private-release hosted-attestation boundary")
reject_text(ci_workflow "actions/attest@"
    "private-release hosted-attestation boundary")

foreach(required_authentication_text IN ITEMS
        "GitHub Enterprise Cloud"
        "public transparency log"
        "workflow_dispatch"
        "authenticate_release"
        "G-PROVENANCE-001"
        "30743014345"
        "release_authentication_evidence_smoke"
        "cosign verify-blob"
        "https://token.actions.githubusercontent.com")
    require_text(release_authentication "${required_authentication_text}"
        "release authentication guide")
endforeach()
require_text(release_process "Sigstore Release Authentication Candidate v1"
    "release authentication build policy")
require_text(release_process "id-token: write"
    "release authentication permission policy")

string(REGEX MATCHALL "MPARSER_WARNINGS_AS_ERRORS=ON"
    warning_gate_matches "${ci_workflow}")
list(LENGTH warning_gate_matches warning_gate_count)
if(NOT warning_gate_count EQUAL 8)
    message(FATAL_ERROR
        "all eight first-party CI configure paths must enable "
        "warnings-as-errors; found ${warning_gate_count}")
endif()
string(REGEX MATCHALL
    "\"MPARSER_WARNINGS_AS_ERRORS\": \"ON\""
    preset_warning_matches "${cmake_presets}")
list(LENGTH preset_warning_matches preset_warning_count)
if(NOT preset_warning_count EQUAL 3)
    message(FATAL_ERROR
        "all checked-in configure presets must enable warnings-as-errors")
endif()
require_text(build_guide "MPARSER_WARNINGS_AS_ERRORS"
    "first-party compiler warning policy")
require_text(release_process "MPARSER_WARNINGS_AS_ERRORS=ON"
    "release compiler warning gate")

require_text(performance_guide "mparser_performance_evidence"
    "performance evidence guide")
require_text(performance_guide "emulated=false"
    "performance evidence native-hardware boundary")
require_text(performance_guide "cross_platform_performance_evidence_smoke"
    "accepted cross-platform performance evidence gate")
require_text(performance_guide "performance_environment_smoke"
    "CPU environment identity evidence gate")
require_text(release_process
    "open Must-have set to be empty"
    "release readiness blocker boundary")
reject_text(release_process
    "exactly authenticated provenance with"
    "closed authenticated-provenance blocker")
reject_text(release_process "cross-platform reliability evidence"
    "closed reliability documentation")
reject_text(release_process "cross-platform package/documentation"
    "closed package/documentation evidence")
reject_text(compatibility_matrix "Final cross-platform archives"
    "closed release-package evidence")
reject_text(embedding_c_api "broader reliability evidence"
    "closed embedding reliability evidence")
reject_text(architecture
    "reliability and sanitizer evidence, performance/resource baselines, documentation, packaging"
    "closed architecture evidence")

require_text(cross_platform_validation "30684969401"
    "cross-platform candidate run identity")
require_text(cross_platform_validation
    "f34d8d9e00816341a01df406c2e315164886c1cc"
    "cross-platform candidate revision identity")
require_text(cross_platform_validation
    "30691616946"
    "cross-platform performance run identity")
require_text(cross_platform_validation
    "85685b88f8f8eb4e89b03abf53aa16dbbe60c68c"
    "cross-platform performance revision identity")
require_text(cross_platform_validation
    "30743014345"
    "cross-platform authentication run identity")
require_text(cross_platform_validation
    "open Must-have set to empty"
    "cross-platform remaining Must-have boundary")

string(JSON authentication_evidence_version GET
    "${release_authentication_evidence}" candidate version)
string(JSON authentication_evidence_tag GET
    "${release_authentication_evidence}" candidate tag)
string(JSON authentication_evidence_revision GET
    "${release_authentication_evidence}" candidate revision)
string(JSON authentication_evidence_run GET
    "${release_authentication_evidence}" workflow run_id)
string(JSON authentication_evidence_subjects GET
    "${release_authentication_evidence}"
    independent_verification verified_subjects)
if(NOT authentication_evidence_version STREQUAL "0.90.1" OR
   NOT authentication_evidence_tag STREQUAL "v0.90.1" OR
   NOT authentication_evidence_revision STREQUAL
       "5763b4752657c54ee5baeaf645a4249b4c5cc8ba" OR
   NOT authentication_evidence_run EQUAL 30743014345 OR
   NOT authentication_evidence_subjects EQUAL 10)
    message(FATAL_ERROR
        "release authentication evidence identity drifted")
endif()

set(indexed_documents ${required_documents})
list(REMOVE_ITEM indexed_documents docs/README.md)
foreach(relative_path IN LISTS indexed_documents)
    string(REPLACE "docs/" "" document_name "${relative_path}")
    require_text(documentation_index
        "(${document_name})"
        "documentation index")
endforeach()

require_text(project_readme
    "(docs/README.md)"
    "project README")
require_text(project_readme
    "(docs/user-manual.md)"
    "project README")
require_text(project_readme
    "(docs/support-matrix.md)"
    "project README")
require_text(project_readme
    "(docs/migration-v1.0.md)"
    "project README")
require_text(project_readme
    "(docs/release-evidence/v0.90.1-authentication/README.md)"
    "project README")

require_text(user_manual
    "complete MATLAB or toolbox compatibility"
    "user manual scope")
require_text(support_matrix
    "compatibility-matrix.json"
    "support matrix authority")
require_text(support_matrix
    "Post-v1.0"
    "support matrix deferred scope")
require_text(jit_guide
    "bytecode VM remains the semantic authority"
    "JIT fallback contract")
require_text(runtime_guide
    "Zero means unlimited"
    "runtime resource contract")
require_text(runtime_guide
    "not an operating-system sandbox"
    "runtime security boundary")
require_text(migration_guide
    "The undocumented pre-v1 `--run-interpreter` alias is removed"
    "migration removed-mode rule")
require_text(build_guide
    "third_party/sljit"
    "offline native-JIT build rule")

string(JSON cli_schema_major GET "${cli_contract}" schema major)
string(JSON cli_schema_minor GET "${cli_contract}" schema minor)
if(NOT cli_schema_major EQUAL 1 OR NOT cli_schema_minor EQUAL 0)
    message(FATAL_ERROR
        "release documentation expects CLI contract 1.0")
endif()

string(JSON production_mode GET "${cli_contract}" production mode)
string(JSON production_jit GET "${cli_contract}" production jit_option)
string(JSON production_machine GET
    "${cli_contract}" production machine_option)
require_text(cli_reference "${production_mode}" "CLI reference")
require_text(cli_reference "${production_jit}" "CLI reference")
require_text(cli_reference "${production_machine}" "CLI reference")

set(documented_contract_options)
string(JSON stable_option_count LENGTH
    "${cli_contract}" production stable_options)
math(EXPR stable_option_last "${stable_option_count} - 1")
foreach(index RANGE 0 ${stable_option_last})
    string(JSON option GET
        "${cli_contract}" production stable_options ${index})
    require_text(cli_reference "${option}" "CLI stable option table")
    list(APPEND documented_contract_options "${option}")
endforeach()

string(JSON diagnostic_mode_count LENGTH
    "${cli_contract}" diagnostic_modes modes)
math(EXPR diagnostic_mode_last "${diagnostic_mode_count} - 1")
foreach(index RANGE 0 ${diagnostic_mode_last})
    string(JSON mode GET
        "${cli_contract}" diagnostic_modes modes ${index})
    require_text(cli_reference "${mode}" "CLI diagnostic mode table")
endforeach()

string(JSON backend_option GET
    "${cli_contract}" diagnostic_modes backend_option)
require_text(cli_reference "${backend_option}" "CLI backend option")

foreach(rule IN ITEMS repeatable single_occurrence)
    string(JSON option_count LENGTH
        "${cli_contract}" option_rules ${rule})
    math(EXPR option_last "${option_count} - 1")
    foreach(index RANGE 0 ${option_last})
        string(JSON option GET
            "${cli_contract}" option_rules ${rule} ${index})
        if(option MATCHES "^--")
            require_text(cli_reference "${option}" "CLI option rules")
        endif()
    endforeach()
endforeach()

string(JSON utility_count LENGTH "${cli_contract}" utility_commands)
math(EXPR utility_last "${utility_count} - 1")
foreach(index RANGE 0 ${utility_last})
    string(JSON utility MEMBER
        "${cli_contract}" utility_commands ${index})
    require_text(cli_reference "${utility}" "CLI utility commands")
endforeach()

string(JSON removed_count LENGTH "${cli_contract}" removed_before_v1)
math(EXPR removed_last "${removed_count} - 1")
foreach(index RANGE 0 ${removed_last})
    string(JSON removed_name GET
        "${cli_contract}" removed_before_v1 ${index} name)
    string(JSON replacement GET
        "${cli_contract}" removed_before_v1 ${index} replacement)
    require_text(cli_reference "${removed_name}" "CLI removed-mode migration")
    require_text(cli_reference "${replacement}" "CLI removed-mode migration")
    require_text(migration_guide "${removed_name}" "v1 migration guide")
    require_text(migration_guide "${replacement}" "v1 migration guide")
endforeach()

set(mparser_command)
if(DEFINED MPARSER_EMULATOR AND NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND mparser_command ${MPARSER_EMULATOR})
endif()
list(APPEND mparser_command "${MPARSER_EXECUTABLE}")

execute_process(
    COMMAND ${mparser_command} --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_output
    ERROR_VARIABLE help_error)
if(NOT "${help_result}" STREQUAL "0" OR NOT help_error STREQUAL "")
    message(FATAL_ERROR
        "mparser --help failed (${help_result})\n"
        "stdout:\n${help_output}\n"
        "stderr:\n${help_error}")
endif()

set(live_contract_tokens
    "${production_mode}"
    "${production_jit}"
    "${production_machine}"
    "${backend_option}")
list(APPEND live_contract_tokens ${documented_contract_options})
foreach(index RANGE 0 ${diagnostic_mode_last})
    string(JSON mode GET
        "${cli_contract}" diagnostic_modes modes ${index})
    list(APPEND live_contract_tokens "${mode}")
endforeach()
foreach(index RANGE 0 ${utility_last})
    string(JSON utility MEMBER
        "${cli_contract}" utility_commands ${index})
    list(APPEND live_contract_tokens "${utility}")
endforeach()
list(REMOVE_DUPLICATES live_contract_tokens)

foreach(token IN LISTS live_contract_tokens)
    string(REGEX REPLACE "=.*$" "" option_name "${token}")
    string(FIND "${help_output}" "${option_name}" option_at)
    if(option_at EQUAL -1)
        message(FATAL_ERROR
            "live --help is missing CLI contract option: ${option_name}")
    endif()
endforeach()

execute_process(
    COMMAND ${mparser_command}
        --run --jit=off "${ENTRY_SOURCE}"
    RESULT_VARIABLE sample_result
    OUTPUT_VARIABLE sample_output
    ERROR_VARIABLE sample_error)
if(NOT "${sample_result}" STREQUAL "0" OR
   NOT sample_output MATCHES "summary = 705" OR
   NOT sample_error STREQUAL "")
    message(FATAL_ERROR
        "documented production sample failed (${sample_result})\n"
        "stdout:\n${sample_output}\n"
        "stderr:\n${sample_error}")
endif()

execute_process(
    COMMAND ${mparser_command}
        --run --jit=off --result-format=json-v1 "${ENTRY_SOURCE}"
    RESULT_VARIABLE machine_result
    OUTPUT_VARIABLE machine_output
    ERROR_VARIABLE machine_error)
if(NOT "${machine_result}" STREQUAL "0" OR
   NOT machine_error STREQUAL "")
    message(FATAL_ERROR
        "documented machine sample failed (${machine_result})\n"
        "stdout:\n${machine_output}\n"
        "stderr:\n${machine_error}")
endif()
string(JSON machine_schema_name GET "${machine_output}" protocol name)
string(JSON machine_schema_major GET "${machine_output}" protocol major)
string(JSON machine_status GET "${machine_output}" status)
if(NOT machine_schema_name STREQUAL "mparser.result" OR
   NOT machine_schema_major EQUAL 1 OR
   NOT machine_status STREQUAL "succeeded")
    message(FATAL_ERROR
        "documented machine sample returned an invalid protocol result")
endif()

message(STATUS
    "MParser release documentation validated: CLI 1.0, manuals, "
    "production sample, and mparser.result 1.x")
