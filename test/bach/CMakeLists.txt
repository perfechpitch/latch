# 每个 .cpp 一个 CTest 目标，目标名取文件名。
file(GLOB_RECURSE BACH_TEST_SRCS "*.cpp")

foreach(BACH_TEST_SRC ${BACH_TEST_SRCS})
  get_filename_component(TEST_NAME ${BACH_TEST_SRC} NAME_WE)
  add_executable(${TEST_NAME} ${BASE_SRC} ${BACH_TEST_SRC})
  target_include_directories(${TEST_NAME} PUBLIC ${LATCH_ROOT_DIR}/src)
  # 用例之间共用的头按 test/... 引，与 src 那一套路径分开。
  target_include_directories(${TEST_NAME} PUBLIC ${LATCH_ROOT_DIR})
  # RV core 的测试要读 src/rv32 的功能模型，也要按绝对路径找 kernel 镜像
  target_include_directories(${TEST_NAME} PUBLIC ${LATCH_ROOT_DIR}/src/rv32)
  target_compile_definitions(${TEST_NAME}
                             PUBLIC LATCH_SOURCE_DIR="${LATCH_ROOT_DIR}")
  target_link_libraries(${TEST_NAME} latch_base_static)
  target_link_libraries(${TEST_NAME} rv32_static)
  target_link_libraries(${TEST_NAME} gtest gtest_main)
  target_link_libraries(${TEST_NAME} spdlog)
  target_link_libraries(${TEST_NAME} pthread dl)
  target_link_libraries(${TEST_NAME} sqlite3)
  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# 硬件配置生成器与参考实现都是 Python 写的，各挂一条 CTest。
# 参考实现那一条还查比对向量与代码同步：向量由 C++ 那一侧读进来做逐 bit 比对，
# 不同步的话比的就是旧结果。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME hwconfig
           COMMAND ${Python3_EXECUTABLE}
                   ${LATCH_ROOT_DIR}/src/bach/compiler/selftest_hwconfig.py)
  add_test(NAME reference
           COMMAND ${Python3_EXECUTABLE}
                   ${LATCH_ROOT_DIR}/src/bach/compiler/reference/selftest.py)
endif()
