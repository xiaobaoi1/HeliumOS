# 工具链
ASM = nasm
CC = gcc
LD = ld
QEMU = qemu-system-i386
GRUB_MKRESCUE = grub-mkrescue

# 编译参数（32位，独立环境，无标准库，生成调试信息）
CFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -Wall -Wextra -g -I src/include
ASMFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

# 目录
SRC_DIR = src
BUILD_DIR = build
BOOT_DIR = $(SRC_DIR)/boot
KERNEL_DIR = $(SRC_DIR)/kernel

# 目标文件
OBJS = $(BUILD_DIR)/start.o \
       $(BUILD_DIR)/kmain.o \
       $(BUILD_DIR)/screen.o

# 最终内核
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
ISO = HeliumOS.iso

# 默认目标
all: $(ISO)

# 编译汇编
$(BUILD_DIR)/start.o: $(BOOT_DIR)/start.asm
	@mkdir -p $(BUILD_DIR)
	$(ASM) $(ASMFLAGS) $< -o $@

# 编译 C 文件
$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# 链接内核
$(KERNEL_ELF): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# 生成 ISO（包含 GRUB 和内核）
$(ISO): $(KERNEL_ELF)
	@mkdir -p $(BUILD_DIR)/iso_root/boot/grub
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso_root/boot/
	@echo "set timeout=5" > $(BUILD_DIR)/iso_root/boot/grub/grub.cfg
	@echo "menuentry \"HeliumOS\" {" >> $(BUILD_DIR)/iso_root/boot/grub/grub.cfg
	@echo "  multiboot2 /boot/kernel.elf" >> $(BUILD_DIR)/iso_root/boot/grub/grub.cfg
	@echo "}" >> $(BUILD_DIR)/iso_root/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(ISO) $(BUILD_DIR)/iso_root

# 运行 QEMU
run: $(ISO)
	$(QEMU) -cdrom $(ISO) -serial stdio -s -S

# 调试（开启 GDB 端口）
debug: $(ISO)
	$(QEMU) -cdrom $(ISO) -serial stdio -s -S

# 清理
clean:
	rm -rf $(BUILD_DIR) $(ISO)

.PHONY: all run debug clean