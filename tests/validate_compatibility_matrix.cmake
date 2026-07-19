cmake_minimum_required(VERSION 3.20)

foreach(required IN ITEMS MATRIX PROJECT_ROOT TEST_REGISTRY EXPECTED_VERSION)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "compatibility matrix validator requires ${required}")
    endif()
endforeach()

file(READ "${MATRIX}" matrix_json)
file(READ "${TEST_REGISTRY}" test_registry)

string(JSON schema_version GET "${matrix_json}" schema_version)
if(NOT schema_version EQUAL 1)
    message(FATAL_ERROR "unsupported compatibility matrix schema: ${schema_version}")
endif()

string(JSON matrix_version GET "${matrix_json}" version)
if(NOT matrix_version STREQUAL EXPECTED_VERSION)
    message(FATAL_ERROR
        "compatibility matrix version ${matrix_version} does not match project ${EXPECTED_VERSION}")
endif()

set(allowed_statuses supported partial unsupported planned)
set(allowed_priorities must-have should-have post-v1.0)
set(allowed_tier_states
    supported partial fallback unsupported planned not-applicable)
set(required_tiers parser semantic hir bytecode production typed native)
set(seen_ids)

string(JSON feature_count LENGTH "${matrix_json}" features)
if(feature_count LESS 20)
    message(FATAL_ERROR
        "compatibility matrix must cover at least 20 feature contracts")
endif()
math(EXPR feature_last "${feature_count} - 1")

foreach(index RANGE 0 ${feature_last})
    string(JSON id GET "${matrix_json}" features ${index} id)
    string(JSON feature GET "${matrix_json}" features ${index} feature)
    string(JSON status GET "${matrix_json}" features ${index} status)
    string(JSON priority GET "${matrix_json}" features ${index} priority)
    string(JSON limits GET "${matrix_json}" features ${index} limits)

    if(id STREQUAL "" OR feature STREQUAL "" OR limits STREQUAL "")
        message(FATAL_ERROR
            "feature ${index} requires nonempty id, feature, and limits")
    endif()
    list(FIND seen_ids "${id}" duplicate_index)
    if(NOT duplicate_index EQUAL -1)
        message(FATAL_ERROR "duplicate compatibility id: ${id}")
    endif()
    list(APPEND seen_ids "${id}")

    list(FIND allowed_statuses "${status}" status_index)
    if(status_index EQUAL -1)
        message(FATAL_ERROR "feature ${id} has invalid status: ${status}")
    endif()
    list(FIND allowed_priorities "${priority}" priority_index)
    if(priority_index EQUAL -1)
        message(FATAL_ERROR "feature ${id} has invalid priority: ${priority}")
    endif()

    foreach(tier IN LISTS required_tiers)
        string(JSON tier_state GET
            "${matrix_json}" features ${index} tiers ${tier})
        list(FIND allowed_tier_states "${tier_state}" tier_state_index)
        if(tier_state_index EQUAL -1)
            message(FATAL_ERROR
                "feature ${id} has invalid ${tier} state: ${tier_state}")
        endif()
    endforeach()

    string(JSON source_count LENGTH "${matrix_json}" features ${index} sources)
    if(source_count LESS 1)
        message(FATAL_ERROR "feature ${id} has no source references")
    endif()
    math(EXPR source_last "${source_count} - 1")
    foreach(source_index RANGE 0 ${source_last})
        string(JSON source GET
            "${matrix_json}" features ${index} sources ${source_index})
        if(NOT EXISTS "${PROJECT_ROOT}/${source}")
            message(FATAL_ERROR
                "feature ${id} references missing source: ${source}")
        endif()
    endforeach()

    string(JSON evidence_count LENGTH
        "${matrix_json}" features ${index} evidence)
    if((status STREQUAL "supported" OR status STREQUAL "partial") AND
       evidence_count LESS 1)
        message(FATAL_ERROR
            "feature ${id} claims ${status} without executable evidence")
    endif()
    if(evidence_count GREATER 0)
        math(EXPR evidence_last "${evidence_count} - 1")
        foreach(evidence_index RANGE 0 ${evidence_last})
            string(JSON evidence GET
                "${matrix_json}" features ${index} evidence ${evidence_index})
            string(FIND "${test_registry}" "NAME ${evidence}" registered_at)
            if(registered_at EQUAL -1)
                message(FATAL_ERROR
                    "feature ${id} references unregistered test: ${evidence}")
            endif()
        endforeach()
    endif()
endforeach()

string(JSON gap_count LENGTH "${matrix_json}" gaps)
if(gap_count LESS 10)
    message(FATAL_ERROR
        "compatibility matrix must retain at least 10 explicit gap contracts")
endif()
math(EXPR gap_last "${gap_count} - 1")
set(allowed_gap_states open in-progress deferred)
set(allowed_framework_impacts
    additive contract-extension representation-change none)

foreach(index RANGE 0 ${gap_last})
    string(JSON id GET "${matrix_json}" gaps ${index} id)
    string(JSON priority GET "${matrix_json}" gaps ${index} priority)
    string(JSON state GET "${matrix_json}" gaps ${index} state)
    string(JSON target GET "${matrix_json}" gaps ${index} target)
    string(JSON impact GET "${matrix_json}" gaps ${index} framework_impact)
    string(JSON requirement GET "${matrix_json}" gaps ${index} requirement)
    string(JSON exit_evidence GET "${matrix_json}" gaps ${index} exit_evidence)

    list(FIND seen_ids "${id}" duplicate_index)
    if(id STREQUAL "" OR NOT duplicate_index EQUAL -1)
        message(FATAL_ERROR "missing or duplicate gap id: ${id}")
    endif()
    list(APPEND seen_ids "${id}")
    list(FIND allowed_priorities "${priority}" priority_index)
    list(FIND allowed_gap_states "${state}" state_index)
    list(FIND allowed_framework_impacts "${impact}" impact_index)
    if(priority_index EQUAL -1 OR state_index EQUAL -1 OR
       impact_index EQUAL -1 OR target STREQUAL "" OR
       requirement STREQUAL "" OR exit_evidence STREQUAL "")
        message(FATAL_ERROR "gap ${id} has an invalid or empty contract field")
    endif()
endforeach()

string(FIND "${test_registry}" "PRIVATE /UNDEBUG" msvc_assertions)
string(FIND "${test_registry}" "PRIVATE -UNDEBUG" portable_assertions)
if(msvc_assertions EQUAL -1 OR portable_assertions EQUAL -1)
    message(FATAL_ERROR
        "optimized smoke tests must retain assertions on MSVC and GCC/Clang")
endif()

message(STATUS
    "validated ${feature_count} compatibility features and ${gap_count} gaps for v${matrix_version}")
