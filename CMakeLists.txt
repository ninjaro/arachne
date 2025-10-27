cmake_minimum_required(VERSION 3.21)
project(arachne LANGUAGES CXX)

option(BUILD_TESTS "Build unit tests (GTest)" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(cpr REQUIRED)
find_package(nlohmann_json REQUIRED)
#find_package(SQLite3 REQUIRED)

add_library(arachne_lib STATIC
        src/arachne.cpp
        src/ariadne.cpp
        src/penelope.cpp
        src/pheidippides.cpp
        src/rng.cpp
)
target_include_directories(arachne_lib PUBLIC include)
target_link_libraries(arachne_lib PUBLIC cpr::cpr nlohmann_json::nlohmann_json sqlite3)

add_executable(arachne_app
        src/main.cpp
)
target_link_libraries(arachne_app PRIVATE arachne_lib)

include(CTest)
if (BUILD_TESTS)
    find_package(GTest REQUIRED)
    add_executable(unit_tests
            tests/arachne_tests.cpp
            tests/rng_tests.cpp
    )
    target_link_libraries(unit_tests PRIVATE arachne_lib GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(unit_tests)
endif ()