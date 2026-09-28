cmake_minimum_required(VERSION 3.14)
project(latch_moe_lpu LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(LATCH_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
find_package(Threads REQUIRED)

# This test does not use HP-Socket, SQLite, Ramulator, Python bindings, or
# Verilator. Keep its macOS build independent of those full-project tools.
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LATCH_ROOT}/3rdparties/googletest"
                 "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
add_subdirectory("${LATCH_ROOT}/3rdparties/spdlog-1.11.0"
                 "${CMAKE_BINARY_DIR}/spdlog" EXCLUDE_FROM_ALL)

add_library(moe_lpu_common STATIC
  "${LATCH_ROOT}/src/base/object.cpp"
  "${LATCH_ROOT}/src/base/recorder.cpp"
  "${LATCH_ROOT}/src/base/runtime.cpp"
  "${LATCH_ROOT}/src/libco/co_epoll.cpp"
  "${LATCH_ROOT}/src/libco/co_routine.cpp"
  "${LATCH_ROOT}/src/libco/coctx.cpp"
  "${LATCH_ROOT}/src/libco/co_comm.cpp"
  "${LATCH_ROOT}/src/libco/coctx_swap.S"
  "${LATCH_ROOT}/src/rv32/rv32.cpp"
  "${LATCH_ROOT}/src/rv32/gen/insts_api.cpp")
target_include_directories(moe_lpu_common PUBLIC
  "${LATCH_ROOT}"
  "${LATCH_ROOT}/src"
  "${LATCH_ROOT}/src/base"
  "${LATCH_ROOT}/src/rv32"
  "${LATCH_ROOT}/src/rv32/gen"
  "${LATCH_ROOT}/src/rv32/inst_define")
target_compile_definitions(moe_lpu_common PUBLIC LATCH_SOURCE_DIR="${LATCH_ROOT}")
target_compile_options(moe_lpu_common PRIVATE
  -Wall -Wextra -Wpedantic
  -Werror=return-type -Werror=shadow -Werror=strict-aliasing
  -Wno-reorder -Wno-unused-variable -Wno-unused-parameter -Wno-sign-compare)
target_link_libraries(moe_lpu_common PUBLIC
  gtest spdlog Threads::Threads ${CMAKE_DL_LIBS})

function(add_moe_lpu_test name src)
  add_executable(${name} "${src}")
  target_compile_options(${name} PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=return-type -Werror=shadow -Werror=strict-aliasing
    -Wno-reorder -Wno-unused-variable -Wno-unused-parameter -Wno-sign-compare)
  target_link_libraries(${name} PRIVATE moe_lpu_common gtest_main)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

enable_testing()
file(GLOB_RECURSE BACH_TEST_SRCS "${LATCH_ROOT}/test/bach/*.cpp")
foreach(BACH_TEST_SRC ${BACH_TEST_SRCS})
  get_filename_component(TEST_NAME ${BACH_TEST_SRC} NAME_WE)
  add_moe_lpu_test(${TEST_NAME} "${BACH_TEST_SRC}")
endforeach()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME hwconfig
           COMMAND ${Python3_EXECUTABLE}
                   ${LATCH_ROOT}/src/bach/compiler/selftest_hwconfig.py
           WORKING_DIRECTORY ${LATCH_ROOT})
  add_test(NAME reference
           COMMAND ${Python3_EXECUTABLE}
                   ${LATCH_ROOT}/src/bach/compiler/reference/selftest.py
           WORKING_DIRECTORY ${LATCH_ROOT})
  add_test(NAME tracetto
           COMMAND ${Python3_EXECUTABLE}
                   ${LATCH_ROOT}/src/utils/tracetto/selftest_tracetto.py
           WORKING_DIRECTORY ${LATCH_ROOT})
endif()

add_subdirectory("${LATCH_ROOT}/test/libco" "${CMAKE_BINARY_DIR}/libco_test")
