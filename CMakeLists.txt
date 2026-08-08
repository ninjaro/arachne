cmake_minimum_required(VERSION 3.28)

project(arachne VERSION 2.0.0 LANGUAGES CXX)

option(BUILD_TESTS "Build hermetic tests" ON)
option(BUILD_LEGACY_CLIENT "Build the deprecated Wikidata compatibility API" OFF)
option(ENABLE_NETWORK_TESTS "Build tests that access live external services" OFF)
option(COVERAGE "Enable coverage instrumentation" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer for tests" OFF)

if(COVERAGE AND NOT BUILD_TESTS)
    set(BUILD_TESTS ON CACHE BOOL "Build tests (required by coverage)" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(
        -Og -g -fno-omit-frame-pointer
        -Werror -Wextra -Wpedantic
        -Wcast-align -Wcast-qual -Wconversion
        -Wctor-dtor-privacy -Wenum-compare -Wfloat-equal
        -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
        -Wredundant-decls -Wsign-conversion -Wsign-promo
    )
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -DNDEBUG)
endif()

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(SQLite3 REQUIRED)
find_path(UTF8PROC_INCLUDE_DIR utf8proc.h REQUIRED)
find_library(UTF8PROC_LIBRARY utf8proc REQUIRED)

if(TARGET SQLite3::SQLite3)
    set(ARACHNE_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(ARACHNE_SQLITE_TARGET SQLite::SQLite3)
else()
    message(FATAL_ERROR "FindSQLite3 did not provide an imported target")
endif()

# Actor-neutral primitives and wire contracts have no transport/database link.
add_library(arachne_common STATIC src/common/crypto.cpp)
target_include_directories(arachne_common PUBLIC include)
target_compile_features(arachne_common PUBLIC cxx_std_23)

add_library(arachne_contracts STATIC src/contracts/contracts.cpp)
target_include_directories(arachne_contracts PUBLIC include)
target_link_libraries(arachne_contracts PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(arachne_contracts PUBLIC cxx_std_23)

# Pheidippides is byte transport only and cannot link either graph store.
add_library(pheidippides_transport STATIC
    src/pheidippides/transport.cpp
    src/pheidippides/hardened_transport.cpp
)
target_include_directories(pheidippides_transport PUBLIC include)
target_link_libraries(
    pheidippides_transport
    PUBLIC arachne_common arachne_contracts nlohmann_json::nlohmann_json
    PRIVATE CURL::libcurl
)
target_compile_features(pheidippides_transport PUBLIC cxx_std_23)

# Ariadne owns algorithms, projections, and the static viewer.
add_library(ariadne_engine STATIC
    src/ariadne/candidates.cpp
    src/ariadne/merge_hints.cpp
    src/ariadne/viewer.cpp
)
target_include_directories(
    ariadne_engine
    PUBLIC include
    PRIVATE ${UTF8PROC_INCLUDE_DIR}
)
target_link_libraries(
    ariadne_engine PUBLIC arachne_common arachne_contracts
    nlohmann_json::nlohmann_json
    PRIVATE ${UTF8PROC_LIBRARY}
)
target_compile_features(ariadne_engine PUBLIC cxx_std_23)

# Penelope alone owns graph schemas, staging, activation, and exports.
add_library(penelope_store STATIC
    src/penelope/store.cpp
    src/penelope/inbox.cpp
    src/penelope/merge_hint_store.cpp
)
target_include_directories(penelope_store PUBLIC include)
target_link_libraries(
    penelope_store
    PUBLIC arachne_common arachne_contracts nlohmann_json::nlohmann_json
    PRIVATE ${ARACHNE_SQLITE_TARGET}
)
target_compile_features(penelope_store PUBLIC cxx_std_23)
target_compile_definitions(
    penelope_store PRIVATE ARACHNE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# Arachne owns the external API and operational state, not graph internals.
add_library(arachne_coordinator STATIC src/arachne/coordinator.cpp)
target_include_directories(arachne_coordinator PUBLIC include)
target_link_libraries(
    arachne_coordinator
    PUBLIC arachne_common arachne_contracts nlohmann_json::nlohmann_json
    PRIVATE ${ARACHNE_SQLITE_TARGET}
)
target_compile_features(arachne_coordinator PUBLIC cxx_std_23)

add_library(arachne_core INTERFACE)
target_link_libraries(
    arachne_core INTERFACE
    arachne_coordinator pheidippides_transport ariadne_engine penelope_store
)

add_executable(arachne_app src/main.cpp)
set_target_properties(arachne_app PROPERTIES OUTPUT_NAME arachne)
target_link_libraries(arachne_app PRIVATE arachne_core)
target_compile_definitions(
    arachne_app PRIVATE ARACHNE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# The 1.x Wikidata helper is an explicitly deprecated compatibility target and
# is never linked into the v2 application or workflows.
if(BUILD_LEGACY_CLIENT)
    add_library(arachne_legacy STATIC
        src/arachne.cpp
        src/ariadne.cpp
        src/penelope.cpp
        src/pheidippides.cpp
        src/rng.cpp
        src/utils.cpp
        src/http_client.cpp
    )
    target_include_directories(arachne_legacy PUBLIC include)
    target_link_libraries(
        arachne_legacy
        PUBLIC CURL::libcurl nlohmann_json::nlohmann_json ${ARACHNE_SQLITE_TARGET}
    )
    target_compile_features(arachne_legacy PUBLIC cxx_std_23)
    add_library(arachne_lib ALIAS arachne_legacy)
endif()

add_library(dep_coverage INTERFACE)
if(COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(
            dep_coverage INTERFACE -fprofile-instr-generate -fcoverage-mapping
        )
        target_link_options(
            dep_coverage INTERFACE -fprofile-instr-generate -fcoverage-mapping
        )
        find_program(LLVM_COV llvm-cov REQUIRED)
        find_program(LLVM_PROFDATA llvm-profdata REQUIRED)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(dep_coverage INTERFACE --coverage)
        target_link_options(dep_coverage INTERFACE --coverage)
    else()
        message(FATAL_ERROR "Coverage is supported only with GCC or Clang")
    endif()
endif()

if(COVERAGE)
    target_link_libraries(arachne_common PRIVATE dep_coverage)
    target_link_libraries(arachne_contracts PRIVATE dep_coverage)
    target_link_libraries(pheidippides_transport PRIVATE dep_coverage)
    target_link_libraries(ariadne_engine PRIVATE dep_coverage)
    target_link_libraries(penelope_store PRIVATE dep_coverage)
    target_link_libraries(arachne_coordinator PRIVATE dep_coverage)
endif()

if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(ARACHNE_CORE_TEST_SOURCES
        tests/contracts_tests.cpp
        tests/transport_contract_tests.cpp
        tests/coordinator_tests.cpp
        tests/ariadne_tests.cpp
        tests/viewer_tests.cpp
    )
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/candidate_store_tests.cpp")
        list(APPEND ARACHNE_CORE_TEST_SOURCES tests/candidate_store_tests.cpp)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/product_inbox_tests.cpp")
        list(APPEND ARACHNE_CORE_TEST_SOURCES tests/product_inbox_tests.cpp)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/merge_hints_tests.cpp")
        list(APPEND ARACHNE_CORE_TEST_SOURCES tests/merge_hints_tests.cpp)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/merge_hint_store_tests.cpp")
        list(APPEND ARACHNE_CORE_TEST_SOURCES tests/merge_hint_store_tests.cpp)
    endif()

    add_executable(core_tests ${ARACHNE_CORE_TEST_SOURCES})
    target_link_libraries(
        core_tests PRIVATE arachne_core GTest::gtest_main dep_coverage
    )
    target_compile_definitions(
        core_tests PRIVATE ARACHNE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(ENABLE_ASAN)
        target_compile_options(
            core_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer -g
        )
        target_link_options(core_tests PRIVATE -fsanitize=address)
    endif()

    include(GoogleTest)
    # CTest launches each discovered GoogleTest case in a separate process.
    # Serialize this executable's cases so listener and SQLite resource limits
    # cannot make a highly parallel test run flaky.
    gtest_discover_tests(
        core_tests PROPERTIES RESOURCE_LOCK arachne_core_test_process
    )

    add_test(
        NAME actor_boundary_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_actor_boundaries -v
    )
    add_test(
        NAME intake_adapter_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_intake_adapters -v
    )
    add_test(
        NAME workflow_yaml_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_workflow_yaml -v
    )
    add_test(
        NAME publication_adapter_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_publication_adapter -v
    )
    add_test(
        NAME wikidata_hpc_worker_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest
            tests.test_wikidata_hpc_worker -v
    )
    add_test(
        NAME source_refresh_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_source_refresh -v
    )
    add_test(
        NAME product_v5_migration_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest
            tests.test_product_v5_migration -v
    )
    add_test(
        NAME product_v6_migration_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest
            tests.test_product_v6_migration -v
    )
    add_test(
        NAME viewer_research_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest
            tests.test_viewer_research -v
    )
    add_test(
        NAME operations_cli_tests
        COMMAND ${CMAKE_COMMAND} -E env
            ARACHNE_TEST_BINARY=$<TARGET_FILE:arachne_app>
            ${Python3_EXECUTABLE} -m unittest tests.test_cli -v
    )
    set_tests_properties(
        actor_boundary_tests intake_adapter_tests workflow_yaml_tests
        publication_adapter_tests
        wikidata_hpc_worker_tests source_refresh_tests
        product_v5_migration_tests product_v6_migration_tests
        viewer_research_tests operations_cli_tests
        PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    if(BUILD_LEGACY_CLIENT)
        add_executable(legacy_unit_tests
            tests/arachne_tests.cpp
            tests/rng_tests.cpp
        )
        target_link_libraries(
            legacy_unit_tests PRIVATE arachne_legacy GTest::gtest_main dep_coverage
        )
        gtest_discover_tests(legacy_unit_tests)
    endif()

    if(ENABLE_NETWORK_TESTS AND BUILD_LEGACY_CLIENT)
        add_executable(network_tests
            tests/http_client_tests.cpp
            tests/pheidippides_tests.cpp
            tests/sparql_tests.cpp
        )
        target_link_libraries(
            network_tests PRIVATE arachne_legacy GTest::gtest_main
        )
        gtest_discover_tests(network_tests LABELS network)
    endif()
endif()

if(COVERAGE AND TARGET core_tests)
    set(COVERAGE_DIR "${CMAKE_BINARY_DIR}/coverage")
    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} --build . --target core_tests
        COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIR}
        COMMAND $<TARGET_FILE:core_tests>
        DEPENDS core_tests
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running instrumented hermetic tests"
    )
endif()
