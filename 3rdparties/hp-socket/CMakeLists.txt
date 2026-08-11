

set(HP_SOCKET_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(HP_SOCKET_INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

file(GLOB HP_SOCKET_SOURCES
    ${HP_SOCKET_SRC_DIR}/*.c
    ${HP_SOCKET_SRC_DIR}/*.cpp
    ${HP_SOCKET_SRC_DIR}/linux/*.c
    ${HP_SOCKET_SRC_DIR}/linux/*.c
    ${HP_SOCKET_SRC_DIR}/common/*.cpp
    ${HP_SOCKET_SRC_DIR}/common/http/*.c
    ${HP_SOCKET_SRC_DIR}/common/kcp/*.c
)

file(GLOB HP_SOCKET_HEADERS
    ${HP_SOCKET_INC_DIR}/*.h
    ${HP_SOCKET_INC_DIR}/*.hpp
)

add_library(hp-socket STATIC ${HP_SOCKET_SOURCES} ${HP_SOCKET_HEADERS})
target_include_directories(hp-socket PUBLIC ${HP_SOCKET_INC_DIR})
target_include_directories(hp-socket PUBLIC ${HP_SOCKET_SRC_DIR})
target_link_libraries(hp-socket PRIVATE pthread rt)
target_compile_options(hp-socket PRIVATE -Wno-error=implicit-fallthrough -Wno-error=shadow -fPIC)

