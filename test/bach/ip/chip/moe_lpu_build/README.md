# macOS build for moe_lpu

This focused build contains everything used by `moe_lpu` and omits unrelated
Linux-only or full-project dependencies such as HP-Socket and Ramulator.

From the repository root:

```sh
cmake -S test/bach/ip/chip/moe_lpu_build -B build-moe-lpu \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-moe-lpu -j
ctest --test-dir build-moe-lpu --output-on-failure
```

The test reads the committed `bundle/moe_lpu` kernel images and configuration.
It does not require the separate `compiler/kernel/build` directory.
