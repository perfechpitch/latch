

add_library(sqlite3 STATIC sqlite3.c shell.c)
target_link_libraries(sqlite3 PUBLIC dl)
target_link_options(sqlite3 PUBLIC -fuse-ld=gold -Wl,--disable-new-dtags)
set_target_properties(sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
