cmake_minimum_required(VERSION 3.16)
project(arachne LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(cpr REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(arachne_lib STATIC
    src/arachne.cpp
)
target_include_directories(arachne_lib PUBLIC include)
target_link_libraries(arachne_lib PUBLIC cpr::cpr nlohmann_json::nlohmann_json sqlite3)

add_executable(arachne_app
        src/main.cpp
)
target_link_libraries(arachne_app PRIVATE arachne_lib)
