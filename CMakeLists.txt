cmake_minimum_required(VERSION 3.20)

project(r6
    VERSION 0.1.0
    DESCRIPTION "Experimental PlayStation 1 native recompilation research scaffold"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(r6-core STATIC
    src/bios_services.cpp
    src/mips_interpreter.cpp
    src/software_gpu.cpp
    src/windows_platform_services.cpp
)

target_include_directories(r6-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(r6-diagnostic
    src/main.cpp
)

target_link_libraries(r6-diagnostic PRIVATE r6-core)

add_executable(r6-run
    src/runner_main.cpp
)

target_link_libraries(r6-run PRIVATE r6-core)

add_executable(R6 WIN32
    src/player_main.cpp
)

target_link_libraries(R6 PRIVATE r6-core)
if (WIN32)
    target_link_libraries(R6 PRIVATE gdi32 comdlg32 shell32)
endif()

add_executable(r6-tests
    tests/test_main.cpp
    tests/test_bios_services.cpp
    tests/test_mips_interpreter.cpp
    tests/test_software_gpu.cpp
)

target_include_directories(r6-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

target_link_libraries(r6-tests PRIVATE r6-core)

if (WIN32)
    # GetAsyncKeyState, used by the keyboard controller backend.
    target_link_libraries(r6-core PUBLIC user32)
endif()

if (MSVC)
    target_compile_options(r6-core PRIVATE /W4 /permissive-)
    target_compile_options(r6-diagnostic PRIVATE /W4 /permissive-)
    target_compile_options(r6-run PRIVATE /W4 /permissive-)
    target_compile_options(r6-tests PRIVATE /W4 /permissive-)
    target_compile_options(R6 PRIVATE /W4 /permissive-)
else()
    target_compile_options(r6-core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(r6-diagnostic PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(r6-run PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(r6-tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(R6 PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME r6-tests COMMAND r6-tests)
