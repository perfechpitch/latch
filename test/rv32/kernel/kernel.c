
int add(int a, int b) {
  return a + b;  // 待调试的函数
}

#define uint8_t unsigned char
#define uint16_t unsigned short
#define uint32_t unsigned int

uint8_t load8(uint32_t addr) {
  volatile uint8_t* ptr = addr;
  return *ptr;
}

uint16_t load16(uint32_t addr) {
  volatile uint16_t* ptr = addr;
  return *ptr;
}

uint32_t load32(uint32_t addr) {
  volatile uint32_t* ptr = addr;
  return *ptr;
}

void store8(uint32_t addr, uint8_t data) {
  volatile uint8_t* ptr = addr;
  *ptr = data;
}

void store16(uint32_t addr, uint16_t data) {
  volatile uint16_t* ptr = addr;
  *ptr = data;
}

void store32(uint32_t addr, uint32_t data) {
  volatile uint32_t* ptr = addr;
  *ptr = data;
}

#define NS16550A_UART0_CTRL_ADDR 0x10000000

void send_uart(uint8_t data) {
  uint32_t* ptr = NS16550A_UART0_CTRL_ADDR;
  *ptr = data;
}

void write_sr_t0(uint32_t value) {
  asm volatile("mv t0, %0"  // RISC-V 指令：将 %0 指向的值移动到 t0 寄存器
               :
               : "r"(value)  // 输入：value 放入某个临时寄存器（%0 指代该寄存器）
               : "t0"        // 通知编译器：t0 被手动修改，避免冲突
  );
}

int main() {
  volatile uint8_t* const uart = (volatile uint8_t*)0x10000000;

  for (int i = 0; i < 26; i++) {
    *uart = i + 'A';
  }

  write_sr_t0(0x55);

  for (uint32_t i = 0; i < 0x80000000; i += 0x1000001) {
    store8(i, i & 0xFF);
    store16(i, i & 0xFFFF);
    store32(i, i);
  }

  for (uint32_t i = 0; i < 0x80000000; i += 0x1000001) {
    auto l8 = load8(i);
    if (l8 != (i & 0xFF)) {
      write_sr_t0(0x55);
      return;
    }
    auto l16 = load16(i);
    if (l16 != (i & 0xFFFF)) {
      write_sr_t0(0x55);
      return;
    }
    auto l32 = load32(i);
    if (l32 != (i)) {
      write_sr_t0(0x55);
      return;
    }
    write_sr_t0(0x55);
  }

  write_sr_t0(0x33);

  volatile uint8_t* uart_sts = 0x10000005;
  int result = add(5, 2);
  while (1) {
    result++;
    volatile uint8_t sts = *uart_sts;
    if ((sts & 0x01) != 0) {
      *uart = *uart;
    }
  }
  return 0;
}
