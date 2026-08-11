
# # ----------------------------------------------------------------
# # googlelog.
# # ----------------------------------------------------------------
# if (NOT (TARGET glog))
#   message(STATUS "Finding glog...")
#   set(CMAKE_INSTALL_LIBDIR ${PROJECT_SOURCE_DIR}/lib)
#   set(WITH_UNWIND OFF)
#   set(WITH_GFLAGS OFF) # To prevent a dependency warning.
#   set(BUILD_TESTING OFF)
#   set(BUILD_SHARED_LIBS ON)
#   add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/glog)
#   if (NOT (TARGET glog))
#     message(SEND_ERROR "GLog missing.")
#   endif()
# endif()

# ----------------------------------------------------------------
# googletest (vendored). Provides CMake targets `gtest` / `gtest_main`
# referenced by test/*/CMakeLists.txt. Built static so test binaries
# don't depend on a system / conda libgtest at runtime.
# ----------------------------------------------------------------
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
add_subdirectory(googletest EXCLUDE_FROM_ALL)

# # ----------------------------------------------------------------
# # gflags.
# # ----------------------------------------------------------------
# message(STATUS "Finding gflags...")
# set(GFLAGS_DIR ${CMAKE_CURRENT_LIST_DIR}/gflags)
# if (EXISTS ${GFLAGS_DIR})
#   set(GFLAGS_LIBRARY_INSTALL_DIR ${PROJECT_SOURCE_DIR}/lib)
#   set(GFLAGS_BUILD_SHARED_LIBS ON)
#   set(BUILD_TESTING OFF)
#   add_subdirectory(${GFLAGS_DIR})
# else()
#   message(SEND_ERROR "Gflags missing.")
# endif()


# Build the remaining vendored libraries static by default.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
mark_as_advanced(BUILD_SHARED_LIBS)

add_subdirectory(hp-socket)
target_compile_options(hp-socket PRIVATE -w)

# Header-only / single-translation-unit utilities pulled out of src/utils/.
add_subdirectory(binary_log)
add_subdirectory(inicpp)
add_subdirectory(mio)
add_subdirectory(vcd_writer)

# ----------------------------------------------------------------
# ramulator2 (CMU-SAFARI). Cycle-accurate DRAM simulator. Cloned source from
# the v2.1 branch lives under 3rdparties/ramulator2/. We build only the C++
# `ramulator` library — the Python bindings and the Python-driven C++ codegen
# step are disabled because the generated *.cpp files are committed upstream
# and the Python toolchain is not part of this project's build.
#
# Upstream pulls yaml-cpp and fmt via FetchContent on first configure
# (no system packages required).
# ----------------------------------------------------------------
set(RAMULATOR_PYTHON_BINDINGS OFF CACHE BOOL "Build Ramulator2 Python bindings" FORCE)
# Must be set BEFORE add_subdirectory: after the first configure populates
# ext/yaml-cpp and ext/fmt, subsequent reconfigures otherwise try to `git
# fetch` inside those checkouts (whose .git is stripped) and fail.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
add_subdirectory(ramulator2 EXCLUDE_FROM_ALL)
# Redirect the .so out of the source tree (upstream points it at
# PROJECT_SOURCE_DIR, which would dirty the cloned source).
set_target_properties(ramulator PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/ramulator2
)
# Ramulator2 trips latch-wide -Werror flags (shadowing, unused parameters,
# etc). Silence warnings on the vendored sources only.
foreach(rt ramulator ramulator-base ramulator-frontend ramulator-translation
         ramulator-memorysystem ramulator-dram ramulator-controller)
  if(TARGET ${rt})
    target_compile_options(${rt} PRIVATE -w)
  endif()
endforeach()
