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
