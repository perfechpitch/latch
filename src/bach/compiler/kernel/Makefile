# Bach core 内三个 RV core 的 kernel。
#
# 每份编出 .elf、.hex（Intel HEX，装载用）、.sym（符号表，编译器按它取每笔
# task 的入口地址填进 task_chain 的 TASK_PC）、.dump（反汇编，对着看用）。
# hex 由 gen_hwconfig 拷进 bundle 下各套配置的目录。
#
# 工具链前缀本机是 riscv64-linux-gnu，换机器改 CROSS 一处即可。

CROSS ?= riscv64-linux-gnu-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
NM := $(CROSS)nm

ARCH := -march=rv32im_zicsr -mabi=ilp32
# -fno-pic：裸机上没有装载器也没有 GOT，`la` 要展开成 auipc + addi 的绝对
# 寻址。默认的 PIC 会让它去 GOT 取值，而 GOT 落在 DTCM 里、装载时没进去，
# 取出来的是 0 —— 栈指针成了 0，一用栈就访问到 0xFFFFFFFC。
CFLAGS := $(ARCH) -Os -g0 -nostdlib -nostartfiles -ffreestanding \
          -fno-builtin -fno-pic -Wall -Wextra -I.
LDFLAGS := -T link.ld

# 四样都产在 build 里。装进硬件的那一样（hex）由 gen_hwconfig 拷进 bundle 下各
# 套配置的目录：kernel 是 bundle 的一部分，一套 bundle 要自己带着它。
OUT := build
KINDS := dte mu vu

ELFS := $(patsubst %,$(OUT)/kernel_%.elf,$(KINDS))
DUMPS := $(patsubst %,$(OUT)/kernel_%.dump,$(KINDS))
HEXS := $(patsubst %,$(OUT)/kernel_%.hex,$(KINDS))
SYMS := $(patsubst %,$(OUT)/kernel_%.sym,$(KINDS))

.PHONY: all clean
.PRECIOUS: $(OUT)/kernel_%.elf
all: $(ELFS) $(HEXS) $(SYMS) $(DUMPS)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/kernel_%.elf: kernel_%.c start.s link.ld bach.h | $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< start.s

$(OUT)/kernel_%.hex: $(OUT)/kernel_%.elf
	$(OBJCOPY) -O ihex $< $@

# 只留 task 入口与几个链接脚本符号，一行 "地址 类型 名字"
$(OUT)/kernel_%.sym: $(OUT)/kernel_%.elf
	$(NM) -n $< | grep -E ' [tT] task_| [tT] _start' > $@

$(OUT)/kernel_%.dump: $(OUT)/kernel_%.elf
	$(OBJDUMP) -d -S $< > $@

clean:
	$(RM) -r $(OUT)
