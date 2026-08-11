# rv32 demo kernel
#
# 产物 kernel.bin 是一整块 ILM 镜像，测试直接按 0x80000000 装载。
# 生成物已随仓库提交，只有改了 kernel.cpp / start.S / link.ld 才需要重跑 make。
#
# 默认用 Debian 的 riscv64-linux-gnu 交叉编译器，它带 rv32 的 cc1/cc1plus；
# 换成 riscv64-unknown-elf- 之类的裸机工具链也可以：make CROSS=riscv64-unknown-elf-

CROSS ?= riscv64-linux-gnu-
CC = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
OBJDUMP = $(CROSS)objdump

ARCH = -march=rv32im -mabi=ilp32

# 裸机 C++：没有异常、没有 RTTI、不依赖 libstdc++，
# 静态局部对象也不生成守卫变量和 atexit 注册。
# -fno-pic 很关键：交叉编译器默认开 PIE，那样汇编器会把 la 展开成经 GOT 的
# auipc+lw，而 GOT 落在 DLM 上、在 start.S 搬完 .data 之前根本读不到正确值。
# 关掉 PIC 之后 la 展开成 auipc+addi，取地址不再依赖任何运行期数据。
CXXFLAGS = $(ARCH) -O2 -g -Wall -Wextra \
           -ffreestanding -fno-builtin -fno-exceptions -fno-rtti \
           -fno-threadsafe-statics -fno-use-cxa-atexit \
           -fno-unwind-tables -fno-asynchronous-unwind-tables \
           -fno-stack-protector -fno-common -fno-pic -fno-pie

# rv32 的 libgcc 在这个交叉工具链里没有 multilib，一律不链接运行时库
LDFLAGS = $(ARCH) -nostdlib -nostartfiles -static -no-pie -T link.ld \
          -Wl,--gc-sections -Wl,--no-warn-rwx-segments

ELF = kernel.elf
BIN = kernel.bin
DUMP = kernel.dump
HEX = kernel.hex

.PHONY: all kernel clean

all: kernel

kernel: $(BIN) $(DUMP) $(HEX)

$(ELF): start.S kernel.cpp link.ld
	$(CC) $(CXXFLAGS) $(LDFLAGS) -o $@ start.S kernel.cpp

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(DUMP): $(ELF)
	$(OBJDUMP) -d -S -M numeric,no-aliases $< > $@

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex $< $@

clean:
	$(RM) $(ELF) $(BIN) $(DUMP) $(HEX)
