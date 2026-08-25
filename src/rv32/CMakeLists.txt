# RV32 功能模型（从 logix 迁入）。gen/ 下是代码生成器的产物，本仓不带生成器，按普通源码维护。
set(SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/rv32.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gen/insts_api.cpp
)

add_library(rv32_static STATIC ${SRC})
set_target_properties(rv32_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(rv32_static PUBLIC ${LATCH_ROOT_DIR}/src)
target_include_directories(rv32_static PUBLIC ${LATCH_ROOT_DIR}/src/rv32)
target_include_directories(rv32_static PUBLIC ${LATCH_ROOT_DIR}/src/rv32/gen)
target_include_directories(rv32_static PUBLIC ${LATCH_ROOT_DIR}/src/rv32/inst_define)
target_link_libraries(rv32_static PUBLIC latch_base_static)
target_link_libraries(rv32_static PUBLIC spdlog)
