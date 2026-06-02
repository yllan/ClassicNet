# Host-only unit tests (L-A in DESIGN.md test plan).

function(cn_add_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE classicnet)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

cn_add_test(test_url)
cn_add_test(test_http)
cn_add_test(test_ws)
cn_add_test(test_chunked)
cn_add_test(test_sha1)
cn_add_test(test_base64)
