Libco
===========
Libco is a c/c++ coroutine library that is widely used in WeChat services. It has been running on tens of thousands of machines since 2013.

By linking with libco, you can easily transform synchronous back-end service into coroutine service. The coroutine service will provide out-standing concurrency compare to multi-thread approach. With the system hook, You can easily coding in synchronous way but asynchronous executed.

You can also use co_create/co_resume/co_yield interfaces to create asynchronous back-end service. These interface will give you more control of coroutines.

By libco copy-stack mode, you can easily build a back-end service support tens of millions of tcp connection.
***
### 简介
libco是微信后台大规模使用的c/c++协程库，2013年至今稳定运行在微信后台的数万台机器上。  

libco通过仅有的几个函数接口 co_create/co_resume/co_yield 再配合 co_poll，可以支持同步或者异步的写法，如线程库一样轻松。同时库里面提供了socket族函数的hook，使得后台逻辑服务几乎不用修改逻辑代码就可以完成异步化改造。

作者: sunnyxu(sunnyxu@tencent.com), leiffyli(leiffyli@tencent.com), dengoswei@gmail.com(dengoswei@tencent.com), sarlmolchen(sarlmolchen@tencent.com)

PS: **近期将开源PaxosStore，敬请期待。**

### libco的特性
- 无需侵入业务逻辑，把多进程、多线程服务改造成协程服务，并发能力得到百倍提升;
- 支持CGI框架，轻松构建web服务(New);
- 支持gethostbyname、mysqlclient、ssl等常用第三库(New);
- 可选的共享栈模式，单机轻松接入千万连接(New);
- 完善简洁的协程编程接口
 * 类pthread接口设计，通过co_create、co_resume等简单清晰接口即可完成协程的创建与恢复；
 * __thread的协程私有变量、协程间通信的协程信号量co_signal (New);
 * 语言级别的lambda实现，结合协程原地编写并执行后台异步任务 (New);
 * 基于epoll/kqueue实现的小而轻的网络框架，基于时间轮盘实现的高性能定时器;

### Build

```bash
$ cd /path/to/libco
$ make
```

or use cmake

```bash
$ cd /path/to/libco
$ mkdir build
$ cd build
$ cmake ..
$ make
```

## 协程上下文切换

`co_create / co_resume / co_yield_ct / co_reset` 接口保持不变。CPU 架构由编译器
宏选择，macOS 使用 Mach-O 的下划线符号名，Linux 使用 ELF 符号名。

## macOS Apple Silicon

ARM64 的 `coctx_swap.S` 保存并恢复：

- `x19`～`x28`、帧指针 `x29`、返回地址 `x30`、栈指针 `sp`。
- `d8`～`d15`（AAPCS64 要求保留的 SIMD 寄存器低 64 位）。
- `FPCR` 和 `FPSR`，隔离各协程的舍入模式与浮点异常标志。

不修改 Apple 保留的 `x18`。新栈按 16 字节对齐，入口跳板把两个参数送入
`x0/x1` 后调用入口函数。新协程继承创建现场的浮点环境；入口返回会终止进程，
正常完成仍由现有 `CoRoutineFunc` 标记完成并 yield。上下文结构中的汇编偏移
在 C++ 侧用 `static_assert` 校验。

这是普通 `arm64` ABI 的协作式切换，不支持 `arm64e`，也不支持从信号处理器
抢占或把已创建的协程迁移到另一个 OS 线程。每个线程拥有独立协程环境。
原有 i386/x86_64 路径保留；本次浮点环境隔离仅加在 ARM64 路径。

ABI 依据：
[Apple ARM64 ABI](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms)、
[AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)。

## 独立验证

macOS 安装 Xcode Command Line Tools 后，在仓库根目录运行：

```sh
sh test/libco/run.sh
```

脚本分别以 `-O0/-O2/-O3` 编译并运行，产物默认写入 `build-libco/`。
可设置 `CXX` 和 `LIBCO_TEST_BUILD_DIR`。测试包含 ARM64 寄存器探针、浮点状态、
独立栈/共享栈、递归栈数据、反复 resume/yield、reset 后重跑、嵌套协程和多线程。

也可单独用 CMake 构建，不依赖仿真器、HP-Socket 或 GoogleTest：

```sh
cmake -S test/libco -B build-libco-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-libco-cmake
ctest --test-dir build-libco-cmake --output-on-failure
```

完整项目的 CTest 也注册了 `libco_context`。此实现只解决协程层的 macOS
兼容性；完整仿真器的 HP-Socket 等 Linux 依赖仍需另行移植。
