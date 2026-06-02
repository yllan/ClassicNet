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
    src/cn_sha1.c
    src/cn_base64.c
    src/cn_request.c
)
target_include_directories(classicnet PUBLIC include)
if(CN_HOST)
    target_compile_definitions(classicnet PUBLIC CN_HOST=1)
endif()

# Optional TLS layer (mbedTLS). MBEDTLS_ROOT points at a built mbedTLS tree
# (with include/ and library/*.a) -- e.g. a vendored or locally-built 2.28 LTS.
option(CN_WITH_MBEDTLS "Build the TLS layer against mbedTLS" OFF)
set(MBEDTLS_ROOT "" CACHE PATH "Path to a built mbedTLS (include/ + library/*.a)")
if(CN_WITH_MBEDTLS)
    if(NOT EXISTS "${MBEDTLS_ROOT}/include/mbedtls/ssl.h")
        message(FATAL_ERROR "CN_WITH_MBEDTLS set but MBEDTLS_ROOT is invalid: ${MBEDTLS_ROOT}")
    endif()
    target_sources(classicnet PRIVATE src/cn_tls.c)
    target_compile_definitions(classicnet PUBLIC CN_WITH_MBEDTLS=1)
    target_include_directories(classicnet PUBLIC "${MBEDTLS_ROOT}/include")
    target_link_libraries(classicnet PUBLIC
        "${MBEDTLS_ROOT}/library/libmbedtls.a"
        "${MBEDTLS_ROOT}/library/libmbedx509.a"
        "${MBEDTLS_ROOT}/library/libmbedcrypto.a")
endif()

option(CN_BUILD_TESTS "Build host unit tests" ${CN_HOST})
if(CN_BUILD_TESTS AND CN_HOST)
    enable_testing()
    add_subdirectory(tests)
endif()
