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

find_package(cpr REQUIRED)
#include(FetchContent)
#FetchContent_Declare(
#        cpr
#        GIT_REPOSITORY https://github.com/libcpr/cpr.git
#        GIT_TAG 1.12.0
#)
#FetchContent_MakeAvailable(cpr)
find_package(nlohmann_json REQUIRED)
find_package(SQLite3 REQUIRED)

add_library(arachne_lib STATIC
        src/arachne.cpp
        src/ariadne.cpp
        src/penelope.cpp
        src/pheidippides.cpp
        src/rng.cpp
)
target_include_directories(arachne_lib PUBLIC include)
target_compile_features(arachne_lib PUBLIC cxx_std_23)
target_link_libraries(arachne_lib
        PUBLIC
        cpr::cpr
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
endif ()

if (BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if (GTest_FOUND)
        add_executable(unit_tests
                tests/arachne_tests.cpp
                tests/pheidippides_tests.cpp
                tests/rng_tests.cpp
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
