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
    src/chd_image.cpp
    src/gte.cpp
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

add_executable(r6-player WIN32
    src/player_main.cpp
)

target_link_libraries(r6-player PRIVATE r6-core)
if (WIN32)
    target_link_libraries(r6-player PRIVATE gdi32 comdlg32 shell32)
endif()

# The generated translation unit is a user-local, derived artifact. Point this
# at a file outside the repository to build the native execution slice.
set(R6_GENERATED_TRANSLATION "" CACHE FILEPATH
    "Local generated native translation unit (outside this repository)")
if(R6_GENERATED_TRANSLATION)
    get_filename_component(R6_GENERATED_TRANSLATION_ABS
        "${R6_GENERATED_TRANSLATION}" ABSOLUTE)
    add_executable(r6-native
        src/native_main.cpp
        "${R6_GENERATED_TRANSLATION_ABS}"
    )
    target_link_libraries(r6-native PRIVATE r6-core)
    target_compile_definitions(r6-native PRIVATE R6_HAS_GENERATED_TRANSLATION=1)
endif()

add_executable(r6-tests
    tests/test_main.cpp
    tests/test_bios_services.cpp
    tests/test_chd_image.cpp
    tests/test_gte.cpp
    tests/test_mips_interpreter.cpp
    tests/test_native_runtime.cpp
    tests/test_software_gpu.cpp
)

target_include_directories(r6-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

# The CHD tests read committed fixtures; a direct g++ build from the repository
# root falls back to the same relative path.
target_compile_definitions(r6-tests PRIVATE
    R6_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
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
    target_compile_options(r6-player PRIVATE /W4 /permissive-)
    if(TARGET r6-native)
        target_compile_options(r6-native PRIVATE /W4 /permissive-)
    endif()
else()
    target_compile_options(r6-core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(r6-diagnostic PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(r6-run PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(r6-tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(r6-player PRIVATE -Wall -Wextra -Wpedantic)
    if(TARGET r6-native)
        target_compile_options(r6-native PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

enable_testing()
add_test(NAME r6-tests COMMAND r6-tests)
