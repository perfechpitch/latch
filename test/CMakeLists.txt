enable_testing()
include(GoogleTest)

add_definitions(-DPROJECT_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_definitions(-DPROJECT_TEST_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}")

add_subdirectory(module)
