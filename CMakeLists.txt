cmake_minimum_required(VERSION 3.20)

project(r6
    VERSION 0.1.0
    DESCRIPTION "Experimental PlayStation 1 native recompilation research scaffold"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(r6-diagnostic
    src/main.cpp
    src/windows_platform_services.cpp
)

target_include_directories(r6-diagnostic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (MSVC)
    target_compile_options(r6-diagnostic PRIVATE /W4 /permissive-)
else()
    target_compile_options(r6-diagnostic PRIVATE -Wall -Wextra -Wpedantic)
endif()
