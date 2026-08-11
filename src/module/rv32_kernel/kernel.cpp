// rv32 流水线 demo 用的 C++ 裸机程序
//
// 内容是一趟数组排序加求和，结果经 UART 的 MMIO 寄存器打印出来，
// 最后把退出码写进仿真退出寄存器，流水线看到这次写就停机。
// 顺带验证两件启动时的事：.data 确实从 ILM 搬到了 DLM、.bss 确实清了零。
//
// 运行环境是 freestanding：没有 libc、没有 libstdc++、没有异常和 RTTI，
// 需要的几个 C 库函数在本文件里自带一份。

using size_t = __SIZE_TYPE__;
using uint8_t = unsigned char;
using uint32_t = unsigned int;
using int32_t = int;

// ---------------------------------------------------------------- MMIO 定义

namespace mmio {

constexpr uint32_t kUartTx = 0x10000000;    // 写一个字节即发送
constexpr uint32_t kUartLsr = 0x10000005;   // bit0 = 发送器空闲
constexpr uint32_t kSimExit = 0x10001000;   // 写入值即退出码，写入即停机
constexpr uint32_t kCycleLow = 0x10001008;  // 只读，当前周期计数

inline void Store32(uint32_t addr, uint32_t value) {
  *reinterpret_cast<volatile uint32_t*>(addr) = value;
}

inline uint32_t Load32(uint32_t addr) {
  return *reinterpret_cast<volatile uint32_t*>(addr);
}

inline void Store8(uint32_t addr, uint8_t value) {
  *reinterpret_cast<volatile uint8_t*>(addr) = value;
}

inline uint8_t Load8(uint32_t addr) {
  return *reinterpret_cast<volatile uint8_t*>(addr);
}

}  // namespace mmio

// -------------------------------------------------- freestanding 下的 C 库件

extern "C" {

void* memcpy(void* dst, const void* src, size_t n) {
  uint8_t* d = static_cast<uint8_t*>(dst);
  const uint8_t* s = static_cast<const uint8_t*>(src);
  for (size_t i = 0; i < n; ++i) d[i] = s[i];
  return dst;
}

void* memset(void* dst, int c, size_t n) {
  uint8_t* d = static_cast<uint8_t*>(dst);
  for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(c);
  return dst;
}

}  // extern "C"

// ------------------------------------------------------------------ 串口输出

class Uart {
 public:
  void PutChar(char c) const {
    while ((mmio::Load8(mmio::kUartLsr) & 0x01) == 0) {
    }
    mmio::Store8(mmio::kUartTx, static_cast<uint8_t>(c));
  }

  void Put(const char* s) const {
    for (size_t i = 0; s[i] != '\0'; ++i) PutChar(s[i]);
  }

  void PutLine(const char* s) const {
    Put(s);
    PutChar('\n');
  }

  void PutDec(int32_t v) const {
    if (v < 0) {
      PutChar('-');
      v = -v;
    }
    char buf[12];
    int n = 0;
    do {
      buf[n++] = static_cast<char>('0' + (v % 10));
      v /= 10;
    } while (v != 0);
    while (n > 0) PutChar(buf[--n]);
  }
};

const Uart uart;

// ------------------------------------------------- 全局对象：构造函数会被调用

class BootStamp {
 public:
  // 这个构造函数由 start.S 遍历 .init_array 调用，进 main 之前就已经执行
  BootStamp() : magic(0x5A5A1234) {}
  uint32_t Magic() const { return magic; }

 private:
  uint32_t magic;
};

BootStamp g_boot;

// ------------------------------------------------------ .data 与 .bss 的样本

int32_t g_data[12] = {42, 7, 99, 1, 55, 23, 8, 77, 3, 61, 15, 30};
int32_t g_bss[12];

// ---------------------------------------------------------------- 排序与求和

void BubbleSort(int32_t* arr, int n) {
  for (int i = 0; i < n - 1; ++i) {
    for (int j = 0; j < n - 1 - i; ++j) {
      if (arr[j] > arr[j + 1]) {
        int32_t tmp = arr[j];
        arr[j] = arr[j + 1];
        arr[j + 1] = tmp;
      }
    }
  }
}

int32_t Sum(const int32_t* arr, int n) {
  int32_t acc = 0;
  for (int i = 0; i < n; ++i) acc += arr[i];
  return acc;
}

// ---------------------------------------------------------------------- main

int fails = 0;

void Check(const char* what, int32_t got, int32_t want) {
  if (got == want) {
    uart.Put("  ok   ");
    uart.Put(what);
    uart.Put(" = ");
    uart.PutDec(got);
    uart.PutChar('\n');
    return;
  }
  ++fails;
  uart.Put("  FAIL ");
  uart.Put(what);
  uart.Put(" got ");
  uart.PutDec(got);
  uart.Put(" want ");
  uart.PutDec(want);
  uart.PutChar('\n');
}

extern "C" int main() {
  uart.PutLine("rv32 pipeline demo");

  // 全局对象在 main 之前就构造好了
  Check("boot.magic_ok", g_boot.Magic() == 0x5A5A1234u ? 1 : 0, 1);

  // .data 段确实从 ILM 搬到了 DLM，.bss 确实清了零
  Check("data[0]", g_data[0], 42);
  Check("bss[0]", g_bss[0], 0);

  // 排序：密集的比较分支加上相邻元素的读写
  BubbleSort(g_data, 12);
  Check("sorted[0]", g_data[0], 1);
  Check("sorted[11]", g_data[11], 99);
  Check("sum", Sum(g_data, 12), 421);

  int sorted = 1;
  for (int i = 0; i + 1 < 12; ++i) {
    if (g_data[i] > g_data[i + 1]) sorted = 0;
  }
  Check("is_sorted", sorted, 1);

  // 倒着抄一份进 .bss，再排一次
  for (int i = 0; i < 12; ++i) g_bss[i] = g_data[11 - i];
  BubbleSort(g_bss, 12);
  Check("copy_sorted[0]", g_bss[0], 1);
  Check("copy_sum", Sum(g_bss, 12), 421);

  uart.Put("  cycles ");
  uart.PutDec(static_cast<int32_t>(mmio::Load32(mmio::kCycleLow)));
  uart.PutChar('\n');

  if (fails == 0) {
    uart.PutLine("PASS");
  } else {
    uart.Put("FAIL ");
    uart.PutDec(fails);
    uart.PutChar('\n');
  }
  return fails;
}
