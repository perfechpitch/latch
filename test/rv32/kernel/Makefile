CFLAGS = -I../include -Wall -Wextra
LDFLAGS =

CURDIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

OUT := ./

BIN = $(OUT)/
SHELL_HACK := $(shell mkdir -p $(OUT))


TEST_OBJ = $(OUT)/kernel.elf
TEST_DUMP = $(OUT)/kernel.dump
TEST_BIN = $(OUT)/kernel.bin
TEST_HEX = $(OUT)/kernel.hex


.PHONY: kernel clean

kernel: $(TEST_OBJ) $(TEST_BIN)

$(TEST_OBJ):
	riscv64-unknown-elf-gcc -g0 -gdwarf-2 -march=rv32i -mabi=ilp32 -nostdlib -T link.ld -o $@ kernel.c  start.s
	riscv64-unknown-elf-objdump -d -S $@ > $(TEST_DUMP)
	objcopy -O ihex $(TEST_OBJ) $(TEST_HEX)

$(TEST_BIN): $(TEST_OBJ)
	riscv64-unknown-elf-objcopy -O binary $< $@

clean:
	$(RM) -r $(TEST_OBJ)
	$(RM) -r $(TEST_DUMP)
	$(RM) -r $(TEST_BIN)
	$(RM) -r $(TEST_HEX)

qemu: kernel
	qemu-system-riscv32 -machine virt --trace gdbstub_io_command --trace gdbstub_io_reply -smp 1 -bios none -kernel $(TEST_OBJ) -nographic -s -S

gdb: kernel
	gdb-multiarch -x rv.gdb