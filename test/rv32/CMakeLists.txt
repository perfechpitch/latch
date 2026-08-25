macro(create_rv32_test TEST_SRC)
  get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
  add_executable(${TEST_NAME} ${TEST_SRC})
  target_include_directories(${TEST_NAME} PUBLIC ${LATCH_ROOT_DIR}/src)
  target_link_libraries(${TEST_NAME} rv32_static)
  target_link_libraries(${TEST_NAME} latch_base_static)
  target_link_libraries(${TEST_NAME} gtest gtest_main)
  target_link_libraries(${TEST_NAME} spdlog)
  target_link_libraries(${TEST_NAME} pthread dl)
  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endmacro()

# 裸机内核镜像（test/rv32/kernel，`make` 重新生成 kernel.elf），拷到测试可执行文件旁。
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/kernel/kernel.elf" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

# 逐条指令的语义单测（gen/Rv32UnittestsGen.cpp + inst_define/*.inc 里的 TEST_DEFINE 段）。
create_rv32_test("${LATCH_ROOT_DIR}/src/rv32/gen/Rv32UnittestsGen.cpp")

create_rv32_test("${CMAKE_CURRENT_SOURCE_DIR}/rv32.cpp")
create_rv32_test("${CMAKE_CURRENT_SOURCE_DIR}/scalar.cpp")
create_rv32_test("${CMAKE_CURRENT_SOURCE_DIR}/text_program.cpp")
