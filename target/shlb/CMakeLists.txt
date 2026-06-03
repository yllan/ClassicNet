# Build ClassicNet as a PowerPC CFM shared library (.shlb) plus a demo client.
#
#   cmake -S target/shlb -B build-shlb \
#       -DCMAKE_TOOLCHAIN_FILE=<Retro68-build>/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build-shlb
# Produces ClassicNet.bin (the shlb) and shlbdemo.bin (an app that imports it).
#
cmake_minimum_required(VERSION 3.13)
project(ClassicNetShlb C CXX)

set(CMAKE_C_STANDARD 90)
set(CN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(CN_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

# --- the shared library: portable protocol/crypto + the request state machine.
# No Open Transport here -- the transport (a CNTransport vtable) is supplied by
# the client, so the shlb has no system-library imports and loads cleanly. ---
add_library(ClassicNet SHARED
    ${CN_SRC}/cn_url.c ${CN_SRC}/cn_http.c ${CN_SRC}/cn_ws.c
    ${CN_SRC}/cn_sha1.c ${CN_SRC}/cn_base64.c ${CN_SRC}/cn_request.c)
target_include_directories(ClassicNet PRIVATE ${CN_INC})
set_target_properties(ClassicNet PROPERTIES
    LINK_FLAGS -Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/classicnet.exp)

# XCOFF (libClassicNet.so) -> PEF
add_custom_command(OUTPUT ClassicNet.pef
    COMMAND ${MAKE_PEF} "libClassicNet.so" -o "ClassicNet.pef"
    DEPENDS ClassicNet)

# PEF data fork + cfrg resource fork -> a 'shlb' file
if(REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()
add_custom_command(
    OUTPUT ClassicNet.bin ClassicNet ClassicNet.dsk ClassicNet.ad "%ClassicNet.ad"
    COMMAND ${REZ} ${REZ_FLAGS} ${CMAKE_CURRENT_SOURCE_DIR}/classicnet.r ${REZ_INCLUDE_FLAG}
            -o "ClassicNet.bin" --cc "ClassicNet.dsk" --cc "ClassicNet" --cc "%ClassicNet.ad"
            -t "shlb" -c "????" --data ClassicNet.pef
    DEPENDS ClassicNet.pef)
add_custom_target(ClassicNet_APPL ALL DEPENDS ClassicNet.bin)

# --- demo client: imports the shlb and calls its parsing/crypto API ---
add_application(shlbdemo CONSOLE FILES shlb_demo.c)
target_include_directories(shlbdemo PRIVATE ${CN_INC})
target_link_libraries(shlbdemo ClassicNet)
