set(LIBCO_VERSION   0.5)

enable_language(C ASM)

set(SOURCE_FILES
        co_epoll.cpp

        co_routine.cpp
        coctx.cpp
        co_comm.cpp
        coctx_swap.S)

add_library(colib_static STATIC ${SOURCE_FILES})
add_library(colib_shared SHARED ${SOURCE_FILES})

set_target_properties(colib_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(colib_shared PROPERTIES POSITION_INDEPENDENT_CODE ON)

set_target_properties(colib_static PROPERTIES OUTPUT_NAME colib)
set_target_properties(colib_shared PROPERTIES OUTPUT_NAME colib)

set_target_properties(colib_static PROPERTIES CLEAN_DIRECT_OUTPUT 1)
set_target_properties(colib_shared PROPERTIES CLEAN_DIRECT_OUTPUT 1)

set_target_properties(colib_shared PROPERTIES VERSION ${LIBCO_VERSION} SOVERSION ${LIBCO_VERSION})
