cmake_minimum_required(VERSION 3.20)
project(GekkoPAK VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(gekkopak STATIC
    src/gekkopak.cpp
)

target_include_directories(gekkopak
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(gekkopak PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(gekkopak PRIVATE /W4 /permissive-)
else()
    target_compile_options(gekkopak PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(gekkopak_sim
    sim/main.cpp
)

target_link_libraries(gekkopak_sim PRIVATE gekkopak)
