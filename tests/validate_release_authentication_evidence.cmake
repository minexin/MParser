cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_EVIDENCE_ROOT
        MPARSER_EXPECTED_VERSION
        MPARSER_EXPECTED_TAG
        MPARSER_EXPECTED_REVISION
        MPARSER_EXPECTED_RUN_ID)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "release authentication evidence requires "
            "${required_variable}")
    endif()
endforeach()

set(manifest_path "${MPARSER_EVIDENCE_ROOT}/manifest.json")
set(root_checksums_path "${MPARSER_EVIDENCE_ROOT}/SHA256SUMS")
set(readme_path "${MPARSER_EVIDENCE_ROOT}/README.md")
foreach(required_file IN ITEMS
        "${manifest_path}"
        "${root_checksums_path}"
        "${readme_path}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "release authentication evidence is missing: "
            "${required_file}")
    endif()
endforeach()

set(expected_repository "https://github.com/minexin/MParser")
set(expected_workflow ".github/workflows/ci.yml")
set(expected_identity
    "https://github.com/minexin/MParser/.github/workflows/ci.yml@"
    "refs/tags/${MPARSER_EXPECTED_TAG}")
string(CONCAT expected_identity ${expected_identity})
set(expected_issuer "https://token.actions.githubusercontent.com")
set(expected_run_url
    "https://github.com/minexin/MParser/actions/runs/"
    "${MPARSER_EXPECTED_RUN_ID}")
string(CONCAT expected_run_url ${expected_run_url})

file(READ "${manifest_path}" manifest_json)
string(JSON format_name GET "${manifest_json}" format name)
string(JSON format_major GET "${manifest_json}" format major)
string(JSON format_minor GET "${manifest_json}" format minor)
string(JSON candidate_version GET
    "${manifest_json}" candidate version)
string(JSON candidate_tag GET "${manifest_json}" candidate tag)
string(JSON candidate_repository GET
    "${manifest_json}" candidate repository)
string(JSON candidate_revision GET
    "${manifest_json}" candidate revision)
if(NOT format_name STREQUAL
       "mparser.release-authentication-evidence" OR
   NOT format_major EQUAL 1 OR
   NOT format_minor EQUAL 0 OR
   NOT candidate_version STREQUAL MPARSER_EXPECTED_VERSION OR
   NOT candidate_tag STREQUAL MPARSER_EXPECTED_TAG OR
   NOT candidate_repository STREQUAL expected_repository OR
   NOT candidate_revision STREQUAL MPARSER_EXPECTED_REVISION)
    message(FATAL_ERROR
        "release authentication candidate identity drifted")
endif()

string(JSON workflow_path GET "${manifest_json}" workflow path)
string(JSON workflow_event GET "${manifest_json}" workflow event)
string(JSON workflow_authenticate GET
    "${manifest_json}" workflow authenticate_release)
string(JSON workflow_run_id GET "${manifest_json}" workflow run_id)
string(JSON workflow_run_attempt GET
    "${manifest_json}" workflow run_attempt)
string(JSON workflow_url GET "${manifest_json}" workflow url)
string(JSON workflow_conclusion GET
    "${manifest_json}" workflow conclusion)
string(JSON execution_job_count LENGTH
    "${manifest_json}" workflow execution_jobs)
string(JSON authentication_job_id GET
    "${manifest_json}" workflow authentication_job id)
string(JSON authentication_job_conclusion GET
    "${manifest_json}" workflow authentication_job conclusion)
if(NOT workflow_path STREQUAL expected_workflow OR
   NOT workflow_event STREQUAL "workflow_dispatch" OR
   NOT workflow_authenticate OR
   NOT workflow_run_id EQUAL MPARSER_EXPECTED_RUN_ID OR
   NOT workflow_run_attempt EQUAL 1 OR
   NOT workflow_url STREQUAL expected_run_url OR
   NOT workflow_conclusion STREQUAL "success" OR
   NOT execution_job_count EQUAL 7 OR
   NOT authentication_job_id EQUAL 91586916704 OR
   NOT authentication_job_conclusion STREQUAL "success")
    message(FATAL_ERROR
        "release authentication workflow identity drifted")
endif()

set(expected_execution_jobs
    91583807723
    91583807731
    91583807742
    91583807750
    91583807755
    91583807765
    91583807769)
set(actual_execution_jobs)
math(EXPR execution_job_last "${execution_job_count} - 1")
foreach(job_index RANGE 0 ${execution_job_last})
    string(JSON job_id GET
        "${manifest_json}" workflow execution_jobs ${job_index})
    list(APPEND actual_execution_jobs "${job_id}")
endforeach()
list(SORT expected_execution_jobs)
list(SORT actual_execution_jobs)
if(NOT actual_execution_jobs STREQUAL expected_execution_jobs)
    message(FATAL_ERROR
        "release authentication execution-job set drifted")
endif()

string(JSON signing_identity GET
    "${manifest_json}" signing identity)
string(JSON signing_issuer GET
    "${manifest_json}" signing oidc_issuer)
string(JSON installer_commit GET
    "${manifest_json}" signing cosign_installer_commit)
string(JSON cosign_version GET
    "${manifest_json}" signing cosign_version)
string(JSON bundle_media_type GET
    "${manifest_json}" signing bundle_media_type)
string(JSON signed_subjects GET
    "${manifest_json}" signing signed_subjects)
string(JSON workflow_verified_subjects GET
    "${manifest_json}" signing workflow_verified_subjects)
if(NOT signing_identity STREQUAL expected_identity OR
   NOT signing_issuer STREQUAL expected_issuer OR
   NOT installer_commit STREQUAL
       "6f9f17788090df1f26f669e9d70d6ae9567deba6" OR
   NOT cosign_version STREQUAL "3.0.6" OR
   NOT bundle_media_type STREQUAL
       "application/vnd.dev.sigstore.bundle.v0.3+json" OR
   NOT signed_subjects EQUAL 10 OR
   NOT workflow_verified_subjects EQUAL 10)
    message(FATAL_ERROR
        "release authentication signing contract drifted")
endif()

string(JSON artifact_id GET
    "${manifest_json}" actions_artifact id)
string(JSON artifact_name GET
    "${manifest_json}" actions_artifact name)
string(JSON artifact_size GET
    "${manifest_json}" actions_artifact size_bytes)
string(JSON artifact_sha256 GET
    "${manifest_json}" actions_artifact download_sha256)
string(JSON artifact_platform_count GET
    "${manifest_json}" actions_artifact exact_platform_directories)
string(JSON artifact_file_count GET
    "${manifest_json}" actions_artifact exact_files)
if(NOT artifact_id EQUAL 8843808799 OR
   NOT artifact_name STREQUAL
       "mparser-1.0.0-authenticated-v1.0.0" OR
   NOT artifact_size EQUAL 15104142 OR
   NOT artifact_sha256 STREQUAL
       "9224c5466e39386e9831f6641bc608b49326cc9a6834a6be999e57078d1a74e9" OR
   NOT artifact_platform_count EQUAL 5 OR
   NOT artifact_file_count EQUAL 30)
    message(FATAL_ERROR
        "release authentication Actions artifact identity drifted")
endif()

string(JSON independent_inputs GET
    "${manifest_json}" independent_verification validated_package_inputs)
string(JSON independent_verifier GET
    "${manifest_json}" independent_verification verifier)
string(JSON independent_version GET
    "${manifest_json}" independent_verification verifier_version)
string(JSON independent_mode GET
    "${manifest_json}" independent_verification mode)
string(JSON independent_subjects GET
    "${manifest_json}" independent_verification verified_subjects)
if(NOT independent_inputs EQUAL 5 OR
   NOT independent_verifier STREQUAL "sigstore-python" OR
   NOT independent_version STREQUAL "4.5.0" OR
   NOT independent_mode STREQUAL "offline-bundle" OR
   NOT independent_subjects EQUAL 10)
    message(FATAL_ERROR
        "release authentication independent-verification record drifted")
endif()

function(validate_bundle
        bundle_path expected_bundle_sha256 expected_digest_base64)
    if(NOT EXISTS "${bundle_path}")
        message(FATAL_ERROR
            "release authentication bundle is missing: ${bundle_path}")
    endif()
    file(SHA256 "${bundle_path}" actual_bundle_sha256)
    if(NOT actual_bundle_sha256 STREQUAL expected_bundle_sha256)
        message(FATAL_ERROR
            "release authentication bundle digest mismatch: "
            "${bundle_path}")
    endif()

    file(READ "${bundle_path}" bundle_json)
    string(JSON media_type GET "${bundle_json}" mediaType)
    string(JSON digest_algorithm GET
        "${bundle_json}" messageSignature messageDigest algorithm)
    string(JSON digest_base64 GET
        "${bundle_json}" messageSignature messageDigest digest)
    string(JSON signature GET
        "${bundle_json}" messageSignature signature)
    string(JSON certificate GET
        "${bundle_json}" verificationMaterial certificate rawBytes)
    string(JSON tlog_count LENGTH
        "${bundle_json}" verificationMaterial tlogEntries)
    string(JSON timestamp_count LENGTH
        "${bundle_json}" verificationMaterial timestampVerificationData
        rfc3161Timestamps)
    if(NOT media_type STREQUAL
           "application/vnd.dev.sigstore.bundle.v0.3+json" OR
       NOT digest_algorithm STREQUAL "SHA2_256" OR
       NOT digest_base64 STREQUAL expected_digest_base64 OR
       signature STREQUAL "" OR
       certificate STREQUAL "" OR
       NOT tlog_count EQUAL 1 OR
       NOT timestamp_count EQUAL 1)
        message(FATAL_ERROR
            "release authentication bundle contract mismatch: "
            "${bundle_path}")
    endif()
endfunction()

set(platform_records
    "linux-aarch64|mparser-1.0.0-linux-aarch64|mparser-1.0.0-linux-aarch64.tar.gz|3138173|77e8bdcdc13f626537a141be39f1e3c759150ae7a47f3e0e67bec83fb38eb0ab|d+i9zcE/YmU3oUG+OfHjx1kVCuekfz4OZ77IP7OOsKs=|60a2e2b8bb969fbdeecf297a68d6343a5cf5babfeb25bc874d9959343453cef4|3ff3756ddd0c83911c1df62f2f6244c11d6c0a059518f98ed23ac9acae416a0c|3967|c7fb4f11f5cda84ab3fbe8a083ac64768b929694eaa207c8cc4d113b79f5a601|x/tPEfXNqEqz++igg6xkdouSlpTqogfIzE0RO3n1pgE=|4df9adab508b9cccda918480beeb026003328f80187e113de75a5758b253b2f3|813239ba2f14a55a54fb5085ea15f262b682306a67b58aa6d8ef97d629e56fa2"
    "linux-x86_64|mparser-1.0.0-linux-x86_64|mparser-1.0.0-linux-x86_64.tar.gz|3569170|08fca95bdbf10ebcffa50e47f30235f81cdac8f5bea9916eb02f6567f63aa740|CPypW9vxDrz/pQ5H8wI1+BzayPW+qZFusC9lZ/Y6p0A=|9a6a9d244bf798449387b6ab0840f8a9c4922982d26d7558d9775a1217fd6445|d25c6a339ffb8cde00b2640e3b706de33bdbc841df76cef808d95b0d169782d3|3982|9cba5282b59d491994f43016d95e40a3a9d2cd04f1e2c112fd129d169e0cda75|nLpSgrWdSRmU9DAW2V5Ao6nSzQTx4sES/RKdFp4M2nU=|6ed6816dc3b3703bc3ab6557a75394f083be823197acc95dcf7cf7d841d4aa43|7740fa69fcd65a6219316fe36f4632914107a32f3266734d7d57acd0a5ebbca3"
    "macos-arm64|mparser-1.0.0-macos-arm64|mparser-1.0.0-macos-arm64.tar.gz|2683045|a61cf44973da3810422a7519814e8fd5661ecbac340f346a34216c6f7fcccd61|phz0SXPaOBBCKnUZgU6P1WYey6w0DzRqNCFsb3/MzWE=|ffecd4d89a88e674fc926b2cb561d3b7d2f050c5a26fe27ab911b4210f7590bf|8debab387a5bee218b2e1ac38903842b35eb8dff655baaacd6f4f43777cfdaaf|4010|ed6ebb8787603694383103efd21f791f00ad03b07113909c8647f785cd6ec4c9|7W67h4dgNpQ4MQPv0h95HwCtA7BxE5Cchkf3hc1uxMk=|76a481ae78c21f584d70de34be0a21f973af696c090f2e057322cac5b19a9f09|7fb68666f71db4e0222642af33991bdcd47704a14d1e7238996e8c8775d69c54"
    "macos-x86_64|mparser-1.0.0-macos-x86_64|mparser-1.0.0-macos-x86_64.tar.gz|2900976|12e0f64d8c0f350e8ed4cac2e4b4d60b9d3ceb79b004c553b24d77cf11a5e2f0|EuD2TYwPNQ6O1MrC5LTWC50863mwBMVTsk13zxGl4vA=|11c213ebad90b0bfc92d6968d26c63757035ab9deca08d76f6426e8b73f60f6e|e1049e9fa5afeb5b8669b0fe28c7c226d770fbe9b59f00a294cb5ab9bced7f0e|4012|c2291ba3ee419369f7d35b7e3ac425ebe8f5a5ab987b8891217f7f44454bbe95|wikbo+5Bk2n301t+OsQl6+j1pauYe4iRIX9/REVLvpU=|128bb4d8b7be6834a93e70e4faa5482668c1900d7e22b683dd4e687040d8419e|49ee51c131b69894d075a971f276f53fb021d213eae2de7cfe98d835b8e1a64c"
    "windows-x86_64|mparser-1.0.0-windows-x86_64|mparser-1.0.0-windows-x86_64.zip|2769062|bbe1d57b26ac97bc176c91c3bfe804a2a99763fd47ab2bf88f8a6b0c7028e410|u+HVeyasl7wXbJHDv+gEoqmXY/1Hqyv4j4prDHAo5BA=|7ed87808c660ef4f13540e7cab054487865979fb3b02a0b16bbd01c032bc3f1a|ae6a5e9f0baef013d022089c540ca4c8589e0ad09463ae14fb76c5e647f66040|4145|a2c44344bf2b6638f70a1e7be2f78f176c7b1f496bdb4eeac2d1fda6b595ae0f|osRDRL8rZjj3Ch574vePF2x7H0lr207qwtH9prWVrg8=|203e38d7fcd1f23fe8288b5036f0ae23f94a8052e0c808f294343697aed8b355|48471b9ab06380db2a622f1e57faa956aa3e0124d6375c7ae9269e2acbd371d0")

string(JSON platform_count LENGTH "${manifest_json}" platforms)
list(LENGTH platform_records expected_platform_count)
if(NOT platform_count EQUAL expected_platform_count OR
   NOT platform_count EQUAL 5)
    message(FATAL_ERROR
        "release authentication platform set drifted")
endif()

set(expected_evidence_paths
    "README.md"
    "SHA256SUMS"
    "manifest.json")
set(platform_index 0)
foreach(platform_record IN LISTS platform_records)
    string(REPLACE "|" ";" fields "${platform_record}")
    list(GET fields 0 expected_platform)
    list(GET fields 1 expected_directory)
    list(GET fields 2 expected_archive)
    list(GET fields 3 expected_archive_size)
    list(GET fields 4 expected_archive_sha256)
    list(GET fields 5 expected_archive_base64)
    list(GET fields 6 expected_checksum_sha256)
    list(GET fields 7 expected_archive_bundle_sha256)
    list(GET fields 8 expected_provenance_size)
    list(GET fields 9 expected_provenance_sha256)
    list(GET fields 10 expected_provenance_base64)
    list(GET fields 11 expected_provenance_bundle_sha256)
    list(GET fields 12 expected_checksums_sha256)

    string(JSON platform GET
        "${manifest_json}" platforms ${platform_index} platform)
    string(JSON directory GET
        "${manifest_json}" platforms ${platform_index} directory)
    string(JSON archive_name GET
        "${manifest_json}" platforms ${platform_index} archive name)
    string(JSON archive_size GET
        "${manifest_json}" platforms ${platform_index} archive size_bytes)
    string(JSON archive_sha256 GET
        "${manifest_json}" platforms ${platform_index} archive sha256)
    string(JSON archive_base64 GET
        "${manifest_json}" platforms ${platform_index} archive digest_base64)
    string(JSON archive_checked_in GET
        "${manifest_json}" platforms ${platform_index} archive checked_in)
    string(JSON checksum_sha256 GET
        "${manifest_json}" platforms ${platform_index}
        archive checksum_file_sha256)
    string(JSON archive_bundle_sha256 GET
        "${manifest_json}" platforms ${platform_index}
        archive bundle_sha256)
    string(JSON provenance_name GET
        "${manifest_json}" platforms ${platform_index} provenance name)
    string(JSON provenance_size GET
        "${manifest_json}" platforms ${platform_index}
        provenance size_bytes)
    string(JSON provenance_sha256 GET
        "${manifest_json}" platforms ${platform_index}
        provenance sha256)
    string(JSON provenance_base64 GET
        "${manifest_json}" platforms ${platform_index}
        provenance digest_base64)
    string(JSON provenance_bundle_sha256 GET
        "${manifest_json}" platforms ${platform_index}
        provenance bundle_sha256)
    string(JSON checksums_sha256 GET
        "${manifest_json}" platforms ${platform_index} checksums_sha256)

    if(NOT platform STREQUAL expected_platform OR
       NOT directory STREQUAL expected_directory OR
       NOT archive_name STREQUAL expected_archive OR
       NOT archive_size EQUAL expected_archive_size OR
       NOT archive_sha256 STREQUAL expected_archive_sha256 OR
       NOT archive_base64 STREQUAL expected_archive_base64 OR
       archive_checked_in OR
       NOT checksum_sha256 STREQUAL expected_checksum_sha256 OR
       NOT archive_bundle_sha256 STREQUAL
           expected_archive_bundle_sha256 OR
       NOT provenance_name STREQUAL
           "${expected_archive}.provenance.json" OR
       NOT provenance_size EQUAL expected_provenance_size OR
       NOT provenance_sha256 STREQUAL expected_provenance_sha256 OR
       NOT provenance_base64 STREQUAL expected_provenance_base64 OR
       NOT provenance_bundle_sha256 STREQUAL
           expected_provenance_bundle_sha256 OR
       NOT checksums_sha256 STREQUAL expected_checksums_sha256)
        message(FATAL_ERROR
            "release authentication platform manifest mismatch: "
            "${expected_platform}")
    endif()

    set(platform_root
        "${MPARSER_EVIDENCE_ROOT}/${expected_directory}")
    set(checksum_path
        "${platform_root}/${expected_archive}.sha256")
    set(provenance_path
        "${platform_root}/${expected_archive}.provenance.json")
    set(archive_bundle_path
        "${platform_root}/${expected_archive}.sigstore.json")
    set(provenance_bundle_path
        "${provenance_path}.sigstore.json")
    set(checksums_path "${platform_root}/SHA256SUMS")
    foreach(required_platform_file IN ITEMS
            "${checksum_path}"
            "${provenance_path}"
            "${archive_bundle_path}"
            "${provenance_bundle_path}"
            "${checksums_path}")
        if(NOT EXISTS "${required_platform_file}")
            message(FATAL_ERROR
                "release authentication retained file is missing: "
                "${required_platform_file}")
        endif()
    endforeach()

    file(SHA256 "${checksum_path}" actual_checksum_sha256)
    file(SHA256 "${provenance_path}" actual_provenance_sha256)
    file(SHA256 "${checksums_path}" actual_checksums_sha256)
    if(NOT actual_checksum_sha256 STREQUAL expected_checksum_sha256 OR
       NOT actual_provenance_sha256 STREQUAL
           expected_provenance_sha256 OR
       NOT actual_checksums_sha256 STREQUAL expected_checksums_sha256)
        message(FATAL_ERROR
            "release authentication retained digest mismatch: "
            "${expected_platform}")
    endif()

    file(STRINGS "${checksum_path}" checksum_lines)
    list(LENGTH checksum_lines checksum_line_count)
    if(NOT checksum_line_count EQUAL 1)
        message(FATAL_ERROR
            "release authentication archive sidecar drifted: "
            "${expected_platform}")
    endif()
    list(GET checksum_lines 0 checksum_line)
    if(NOT checksum_line STREQUAL
       "${expected_archive_sha256}  ${expected_archive}")
        message(FATAL_ERROR
            "release authentication archive checksum mismatch: "
            "${expected_platform}")
    endif()

    file(STRINGS "${checksums_path}" platform_checksum_lines)
    list(LENGTH platform_checksum_lines platform_checksum_count)
    list(FIND platform_checksum_lines
        "${expected_archive_sha256}  ${expected_archive}"
        archive_checksum_index)
    string(CONCAT expected_provenance_checksum_line
        "${expected_provenance_sha256}  "
        "${expected_archive}.provenance.json")
    list(FIND platform_checksum_lines
        "${expected_provenance_checksum_line}"
        provenance_checksum_index)
    if(NOT platform_checksum_count EQUAL 2 OR
       archive_checksum_index EQUAL -1 OR
       provenance_checksum_index EQUAL -1)
        message(FATAL_ERROR
            "release authentication platform SHA256SUMS drifted: "
            "${expected_platform}")
    endif()

    file(READ "${provenance_path}" provenance_json)
    string(JSON statement_type GET "${provenance_json}" _type)
    string(JSON predicate_type GET
        "${provenance_json}" predicateType)
    string(JSON subject_count LENGTH "${provenance_json}" subject)
    string(JSON subject_name GET
        "${provenance_json}" subject 0 name)
    string(JSON subject_sha256 GET
        "${provenance_json}" subject 0 digest sha256)
    string(JSON subject_size GET
        "${provenance_json}" subject 0 annotations mparser_sizeBytes)
    string(JSON provenance_version GET
        "${provenance_json}" predicate buildDefinition
        externalParameters projectVersion)
    string(JSON provenance_repository GET
        "${provenance_json}" predicate buildDefinition
        externalParameters repository)
    string(JSON provenance_revision GET
        "${provenance_json}" predicate buildDefinition
        externalParameters revision)
    string(JSON provenance_configuration GET
        "${provenance_json}" predicate buildDefinition
        externalParameters configuration)
    string(JSON provenance_native_jit GET
        "${provenance_json}" predicate buildDefinition
        externalParameters nativeJit)
    string(JSON provenance_packaging GET
        "${provenance_json}" predicate buildDefinition
        externalParameters releasePackaging)
    string(JSON provenance_authentication GET
        "${provenance_json}" predicate runDetails
        mparser_authentication)
    string(JSON provenance_clean GET
        "${provenance_json}" predicate runDetails
        mparser_sourceTreeClean)
    string(JSON source_revision GET
        "${provenance_json}" predicate buildDefinition
        resolvedDependencies 0 digest gitCommit)
    if(NOT statement_type STREQUAL
           "https://in-toto.io/Statement/v1" OR
       NOT predicate_type STREQUAL
           "https://slsa.dev/provenance/v1" OR
       NOT subject_count EQUAL 1 OR
       NOT subject_name STREQUAL expected_archive OR
       NOT subject_sha256 STREQUAL expected_archive_sha256 OR
       NOT subject_size EQUAL expected_archive_size OR
       NOT provenance_version STREQUAL MPARSER_EXPECTED_VERSION OR
       NOT provenance_repository STREQUAL expected_repository OR
       NOT provenance_revision STREQUAL MPARSER_EXPECTED_REVISION OR
       NOT provenance_configuration STREQUAL "Release" OR
       NOT provenance_native_jit OR
       NOT provenance_packaging OR
       NOT provenance_authentication STREQUAL "unsigned" OR
       NOT provenance_clean OR
       NOT source_revision STREQUAL MPARSER_EXPECTED_REVISION)
        message(FATAL_ERROR
            "release authentication provenance contract mismatch: "
            "${expected_platform}")
    endif()

    validate_bundle(
        "${archive_bundle_path}"
        "${expected_archive_bundle_sha256}"
        "${expected_archive_base64}")
    validate_bundle(
        "${provenance_bundle_path}"
        "${expected_provenance_bundle_sha256}"
        "${expected_provenance_base64}")

    list(APPEND expected_evidence_paths
        "${expected_directory}/SHA256SUMS"
        "${expected_directory}/${expected_archive}.provenance.json"
        "${expected_directory}/${expected_archive}.provenance.json.sigstore.json"
        "${expected_directory}/${expected_archive}.sha256"
        "${expected_directory}/${expected_archive}.sigstore.json")
    math(EXPR platform_index "${platform_index} + 1")
endforeach()

file(GLOB_RECURSE actual_evidence_paths
    LIST_DIRECTORIES false
    RELATIVE "${MPARSER_EVIDENCE_ROOT}"
    "${MPARSER_EVIDENCE_ROOT}/*")
list(SORT actual_evidence_paths)
list(SORT expected_evidence_paths)
list(LENGTH actual_evidence_paths actual_evidence_count)
list(LENGTH expected_evidence_paths expected_evidence_count)
if(NOT actual_evidence_paths STREQUAL expected_evidence_paths OR
   NOT actual_evidence_count EQUAL 28 OR
   NOT expected_evidence_count EQUAL 28)
    message(FATAL_ERROR
        "release authentication checked-in evidence set drifted\n"
        "expected: ${expected_evidence_paths}\n"
        "actual: ${actual_evidence_paths}")
endif()

file(STRINGS "${root_checksums_path}" root_checksum_lines)
list(LENGTH root_checksum_lines root_checksum_count)
if(NOT root_checksum_count EQUAL 27)
    message(FATAL_ERROR
        "release authentication root SHA256SUMS count drifted: "
        "${root_checksum_count}")
endif()
foreach(relative_path IN LISTS expected_evidence_paths)
    if(relative_path STREQUAL "SHA256SUMS")
        continue()
    endif()
    file(SHA256
        "${MPARSER_EVIDENCE_ROOT}/${relative_path}"
        retained_sha256)
    list(FIND root_checksum_lines
        "${retained_sha256}  ${relative_path}"
        retained_index)
    if(retained_index EQUAL -1)
        message(FATAL_ERROR
            "release authentication root SHA256SUMS mismatch: "
            "${relative_path}")
    endif()
endforeach()

message(STATUS
    "MParser release authentication evidence validated: "
    "${MPARSER_EXPECTED_TAG}, run ${MPARSER_EXPECTED_RUN_ID}, "
    "5 platforms, 10 Sigstore subjects")
