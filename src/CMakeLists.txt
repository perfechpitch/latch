file(GLOB SRC
  ${LATCH_ROOT_DIR}/src/base/*.cpp
  ${LATCH_ROOT_DIR}/src/libco/*.cpp
  ${LATCH_ROOT_DIR}/src/libco/*.S
)

# co_hook_sys_call.cpp 直接定义 socket/read/write/poll/close/fcntl/setenv 这些
# 强符号，对链接本库的整个进程（含 hp-socket、spdlog）做 libc 符号插入。而 latch
# 从不调 co_enable_hook_sys()，每个 hook 只会白付一次 TLS 查表再转发给 libc，
# 编进来没有收益。libco 自己的 target 本来就没收它，这里是 GLOB 顺手带进来的。
list(FILTER SRC EXCLUDE REGEX "co_hook_sys_call\\.cpp$")

add_library(latch_base_static STATIC ${SRC})
set_target_properties(latch_base_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(latch_base_static PUBLIC ${LATCH_ROOT_DIR}/src)
target_include_directories(latch_base_static PUBLIC ${LATCH_ROOT_DIR}/src/base)
target_link_libraries(latch_base_static PUBLIC hp-socket)
target_link_libraries(latch_base_static PUBLIC mio)

add_subdirectory(libco)
add_subdirectory(bach)
add_subdirectory(rv32)
