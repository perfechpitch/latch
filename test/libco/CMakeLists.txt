cmake_minimum_required(VERSION 3.14)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(libco_context_test LANGUAGES CXX ASM)
  enable_testing()
endif()

find_package(Threads REQUIRED)
set(LIBCO_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src/libco")
add_executable(libco_context
  context.cpp registers.S
  ${LIBCO_SOURCE_DIR}/coctx.cpp
  ${LIBCO_SOURCE_DIR}/coctx_swap.S
  ${LIBCO_SOURCE_DIR}/co_routine.cpp
  ${LIBCO_SOURCE_DIR}/co_epoll.cpp
  ${LIBCO_SOURCE_DIR}/co_comm.cpp)
target_compile_features(libco_context PRIVATE cxx_std_17)
target_include_directories(libco_context PRIVATE ${LIBCO_SOURCE_DIR})
target_link_libraries(libco_context PRIVATE Threads::Threads)
add_test(NAME libco_context COMMAND libco_context)
