# Host-only unit tests (L-A in DESIGN.md test plan).

add_executable(test_url test_url.c)
target_link_libraries(test_url PRIVATE classicnet)
target_include_directories(test_url PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME url COMMAND test_url)
