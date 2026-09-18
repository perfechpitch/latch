#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${LIBCO_TEST_BUILD_DIR:-"$root/build-libco"}
mkdir -p "$out"
for opt in 0 2 3; do
  "${CXX:-c++}" -std=c++17 -O"$opt" -g -pthread -I"$root/src/libco" \
    "$root/test/libco/context.cpp" "$root/test/libco/registers.S" \
    "$root/src/libco/coctx.cpp" "$root/src/libco/coctx_swap.S" \
    "$root/src/libco/co_routine.cpp" "$root/src/libco/co_epoll.cpp" \
    "$root/src/libco/co_comm.cpp" -o "$out/context-O$opt"
  "$out/context-O$opt"
done
