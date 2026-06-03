# On-target (Classic Mac OS / PowerPC) test runner, built with Retro68.
#
# Configure standalone with the Retro68 PPC toolchain, e.g.:
#   cmake -S target -B build-target \
#     -DCMAKE_TOOLCHAIN_FILE=<Retro68-build>/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build-target
# Produces cntest.bin / cntest.APPL to run (or LaunchAPPL) on OS 8/9.

cmake_minimum_required(VERSION 3.13)
project(ClassicNetTarget C CXX)

set(CMAKE_C_STANDARD 90)

add_application(cntest CONSOLE
    FILES
        test_runner.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_url.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_http.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_ws.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_sha1.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_base64.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_request.c
)
target_include_directories(cntest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests)

# HTTP GET over Open Transport (L-D). Needs Apple UI (OT headers) + OT libs.
add_application(cnhttp CONSOLE
    FILES
        http_get.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_ot.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_request.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_http.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_url.c
)
target_compile_definitions(cnhttp PRIVATE CN_WITH_OT=1)
target_include_directories(cnhttp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(cnhttp OpenTransportAppPPC OpenTransportLib OpenTptInternetLib)

# HTTPS over TLS + Open Transport. Needs a PPC-built mbedTLS (MBEDTLS_PPC_ROOT
# points at its build tree with include/ and library/*.a).
if(MBEDTLS_PPC_ROOT)
    add_application(cnhttps CONSOLE
        FILES
            https_get.c
            cn_mac_time.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_ot.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_tls.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_request.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_http.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/cn_url.c
    )
    target_compile_definitions(cnhttps PRIVATE CN_WITH_OT=1 CN_WITH_MBEDTLS=1
        # match the mbedTLS lib's config (Mac time/gmtime/ms_time, see setup-mbedtls.sh)
        MBEDTLS_USER_CONFIG_FILE="mbedtls_userconfig.h")
    # -DCN_VERIFY=ON embeds the CA from scripts/gen-test-pki.sh (target/test_ca.h)
    # and requires certificate verification instead of running insecure.
    option(CN_VERIFY "Require TLS certificate verification (needs target/test_ca.h)" OFF)
    if(CN_VERIFY)
        target_compile_definitions(cnhttps PRIVATE CN_VERIFY=1)
    endif()
    # MBEDTLS_PPC_ROOT = mbedTLS source root; PPC libs are under build-ppc/library.
    target_include_directories(cnhttps PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}            # mbedtls_userconfig.h, test_ca.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${MBEDTLS_PPC_ROOT}/include)
    target_link_libraries(cnhttps
        ${MBEDTLS_PPC_ROOT}/build-ppc/library/libmbedtls.a
        ${MBEDTLS_PPC_ROOT}/build-ppc/library/libmbedx509.a
        ${MBEDTLS_PPC_ROOT}/build-ppc/library/libmbedcrypto.a
        OpenTransportAppPPC OpenTransportLib OpenTptInternetLib
        ThreadsLib)   # cy384's mbedtls yields (YieldToAnyThread) during long crypto
endif()
