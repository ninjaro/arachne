cmake_minimum_required(VERSION 3.28)

project(arachne VERSION 1.0.0 LANGUAGES CXX)

option(BUILD_TESTS "Build unit tests (GTest)" ON)
option(COVERAGE "Enable code coverage instrumentation (tests only)" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer for unit tests" OFF)

if (COVERAGE AND NOT BUILD_TESTS)
    set(BUILD_TESTS ON CACHE BOOL "Build unit tests (forced by COVERAGE)" FORCE)
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(
            -Og -g -fno-omit-frame-pointer
            -Werror -Wextra -Wpedantic
            #            -Wall
            -Wcast-align -Wcast-qual -Wconversion
            -Wctor-dtor-privacy -Wenum-compare -Wfloat-equal
            -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
            -Wredundant-decls -Wsign-conversion -Wsign-promo
    )
elseif (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s")
endif ()

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(SQLite3 REQUIRED)

add_library(arachne_lib STATIC
        src/arachne.cpp
        src/ariadne.cpp
        src/penelope.cpp
        src/pheidippides.cpp

        src/rng.cpp
        src/utils.cpp
        src/http_client.cpp
)
target_compile_features(arachne_lib PUBLIC cxx_std_23)
target_include_directories(arachne_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(arachne_lib
        PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
        FILES
        include/arachne.hpp
        include/ariadne.hpp
        include/penelope.hpp
        include/pheidippides.hpp

        include/rng.hpp
        include/utils.hpp
        include/http_client.hpp
)
target_link_libraries(arachne_lib
        PUBLIC
        CURL::libcurl
        nlohmann_json::nlohmann_json
        SQLite::SQLite3
)

add_executable(arachne_app src/main.cpp)
target_link_libraries(arachne_app PRIVATE arachne_lib)

add_library(dep_coverage INTERFACE)
if (COVERAGE)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(dep_coverage INTERFACE -fprofile-instr-generate -fcoverage-mapping)
        target_link_options(dep_coverage INTERFACE -fprofile-instr-generate -fcoverage-mapping)
        find_program(LLVM_COV llvm-cov REQUIRED)
        find_program(LLVM_PROFDATA llvm-profdata REQUIRED)
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(dep_coverage INTERFACE --coverage -fno-inline -fno-inline-small-functions -fno-default-inline)
        target_link_options(dep_coverage INTERFACE --coverage)
        find_program(GCOVR gcovr REQUIRED)
    else ()
        message(FATAL_ERROR "Coverage only supported on GCC or Clang")
    endif ()
    target_link_libraries(arachne_lib PRIVATE dep_coverage)
endif ()

if (BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if (GTest_FOUND)
        add_executable(unit_tests
                tests/arachne_tests.cpp
                tests/pheidippides_tests.cpp

                tests/rng_tests.cpp
                tests/http_client_tests.cpp
                tests/sparql_tests.cpp
        )
        target_link_libraries(unit_tests PRIVATE arachne_lib GTest::gtest_main dep_coverage)

        if (ENABLE_ASAN)
            target_compile_options(unit_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
            target_link_options(unit_tests PRIVATE -fsanitize=address)
        endif ()

        include(GoogleTest)
        gtest_discover_tests(unit_tests)
    else ()
        message(WARNING "GTest not found; unit tests will not be built")
    endif ()
endif ()

if (COVERAGE)
    set(COVERAGE_DIR "${CMAKE_BINARY_DIR}/../doc/cov")
    set(COVERAGE_IGNORE_REGEX ".*/tests/.*|.*/usr/lib/.*")

    if (TARGET unit_tests)
        add_custom_command(
                TARGET unit_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIR}
        )

        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(PROFRAW_FILE "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.profraw")
            set(PROFDATA_FILE "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.profdata")

            add_custom_target(coverage
                    COMMENT "Generating coverage with llvm-cov"
                    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                    COMMAND ${CMAKE_COMMAND} --build . --target unit_tests
                    COMMAND ${CMAKE_COMMAND} -E env LLVM_PROFILE_FILE=${PROFRAW_FILE} $<TARGET_FILE:unit_tests>
                    COMMAND ${LLVM_PROFDATA} merge -sparse -o ${PROFDATA_FILE} ${PROFRAW_FILE}
                    COMMAND ${LLVM_COV} show $<TARGET_FILE:unit_tests>
                    -instr-profile=${PROFDATA_FILE}
                    -format=html
                    -output-dir=${COVERAGE_DIR}
                    -ignore-filename-regex=${COVERAGE_IGNORE_REGEX}
                    -show-branches=count
                    -Xdemangler=c++filt
                    -show-line-counts
                    -show-regions
                    -show-instantiations
                    -show-expansions
                    -use-color
                    -coverage-watermark=90,60
                    COMMAND ${LLVM_COV} export $<TARGET_FILE:unit_tests>
                    -instr-profile=${PROFDATA_FILE}
                    -format=lcov
                    -ignore-filename-regex=${COVERAGE_IGNORE_REGEX} > ${CMAKE_BINARY_DIR}/coverage.info
                    DEPENDS unit_tests
                    VERBATIM
            )

        elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_custom_target(coverage
                    COMMENT "Generating coverage with gcovr"
                    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                    COMMAND ${CMAKE_COMMAND} --build . --target unit_tests
                    COMMAND $<TARGET_FILE:unit_tests>
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIR}
                    COMMAND ${GCOVR}
                    --root ${CMAKE_SOURCE_DIR}
                    --exclude '${COVERAGE_IGNORE_REGEX}'
                    --html --html-details
                    -o ${COVERAGE_DIR}/index.html
                    DEPENDS unit_tests
            )
        endif ()

    else ()
        message(STATUS "COVERAGE=ON but tests are not built; 'coverage' target will be a no-op.")
    endif ()
endif ()
