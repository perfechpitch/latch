cmake_minimum_required(VERSION 3.1)

if (POLICY CMP0048)
  cmake_policy(SET CMP0048 NEW)
endif (POLICY CMP0048)

if (POLICY CMP0077)
  cmake_policy(SET CMP0077 NEW)
endif (POLICY CMP0077)

project(LATCH)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -Wall -g2 -ggdb")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")
set(LATCH_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_compile_options(-Wall -Wextra -Wunused-function -Wpedantic)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-Werror=return-local-addr -Wno-deprecated-copy -Werror=maybe-uninitialized)
  add_compile_options(-Wno-cast-function-type)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options(-Werror=return-stack-address -Wno-deprecated -Werror=uninitialized)
  add_compile_options(-Wno-bad-function-cast -Wno-gnu-designator )
  add_compile_options(-Wno-inconsistent-missing-override)
  add_compile_options(-Wno-nested-anon-types -Wno-unused-private-field)
  add_compile_options(-Wno-delete-non-abstract-non-virtual-dtor)
endif()

add_compile_options(-Werror=return-type -Werror=shadow -Werror=strict-aliasing
  -Werror=uninitialized)

add_compile_options(-Wno-reorder -Wno-unused-variable -Wno-unused-parameter -Wno-sign-compare)
add_compile_options (-Wno-ignored-qualifiers)

enable_language(C ASM)

message(STATUS "C++ Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "C++ Compiler ID: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "C++ Compiler Version: ${CMAKE_CXX_COMPILER_VERSION}")

message(STATUS "CMAKE_BUILD_TYPE:" ${CMAKE_BUILD_TYPE})
message(STATUS "CMAKE_INSTALL_PREFIX:" ${CMAKE_INSTALL_PREFIX})
set(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" CACHE PATH "" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python3 COMPONENTS Interpreter Development)
message("-- Python3_FOUND:"  ${Python3_FOUND})
message("-- Python3_EXECUTABLE:"  ${Python3_EXECUTABLE})

add_definitions(-DCMAKE_EXPORT_COMPILE_COMMANDS=ON)

add_definitions(-DPROJECT_ROOT_DIR="${CMAKE_CURRENT_BINARY_DIR}")

include_directories(./include/)

add_custom_target(thirdparty)
include(FetchContent)
include(ExternalProject)

add_subdirectory(3rdparties/pybind11)

add_subdirectory(3rdparties/spdlog-1.11.0)
include_directories(${spdlog_SOURCE_DIR}/include)

find_package(Python COMPONENTS Interpreter REQUIRED)

add_subdirectory(./3rdparties/sqlite3)

find_package(verilator HINTS $ENV{VERILATOR_ROOT} ${VERILATOR_ROOT})

include_directories(${VERILATOR_ROOT}/include)
message(VERILATOR_ROOT : ${VERILATOR_ROOT})

add_subdirectory(test)
add_subdirectory(src)

add_subdirectory(3rdparties)
enable_testing()
