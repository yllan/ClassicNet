cmake_minimum_required(VERSION 3.13)
project(ClassicNet C)

#
# ClassicNet builds two ways:
#   * Host (Linux/macOS): portable protocol code, compiled with sanitizers and
#     warnings for fast unit testing and fuzzing. Defines CN_HOST.
#   * Target (Retro68/CodeWarrior): the same src/ plus Mac-specific glue.
#
# C90 keeps the code portable down to the CodeWarrior C89 fallback path.
#
set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_SYSTEM_NAME MATCHES "Retro" OR DEFINED RETRO68_TOOLCHAIN)
    set(CN_TARGET_MAC ON)
else()
    set(CN_HOST ON)
endif()

option(CN_SANITIZE "Enable ASan/UBSan on host builds" ON)

if(CN_HOST)
    add_compile_options(-Wall -Wextra)
    if(CN_SANITIZE)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

add_library(classicnet STATIC
    src/cn_url.c
    src/cn_http.c
    src/cn_ws.c
)
target_include_directories(classicnet PUBLIC include)
if(CN_HOST)
    target_compile_definitions(classicnet PUBLIC CN_HOST=1)
endif()

option(CN_BUILD_TESTS "Build host unit tests" ${CN_HOST})
if(CN_BUILD_TESTS AND CN_HOST)
    enable_testing()
    add_subdirectory(tests)
endif()
