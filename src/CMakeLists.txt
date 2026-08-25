file(GLOB SRC
  ${LATCH_ROOT_DIR}/src/base/*.cpp
  ${LATCH_ROOT_DIR}/src/libco/*.cpp
  ${LATCH_ROOT_DIR}/src/libco/*.S
)

add_library(latch_base_static STATIC ${SRC})
set_target_properties(latch_base_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(latch_base_static PUBLIC ${LATCH_ROOT_DIR}/src)
target_include_directories(latch_base_static PUBLIC ${LATCH_ROOT_DIR}/src/base)
target_link_libraries(latch_base_static PUBLIC hp-socket)
target_link_libraries(latch_base_static PUBLIC mio)

add_subdirectory(libco)
add_subdirectory(bach)
add_subdirectory(rv32)
