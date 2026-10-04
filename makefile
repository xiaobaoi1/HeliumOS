# ====================================================================
# HeliumOS Makefile
# 默认: 生成 ISO 镜像 (便于快速启动)
# 可选: 生成 FAT32 硬盘镜像 (用于测试文件系统写入)
# 头文件管理: 公共头文件位于 src/include，通过 -I 引用
# ====================================================================

# -------------------- 工具链 --------------------
ASM     = nasm
CC      = gcc
LD      = ld
QEMU    = qemu-system-i386
GRUB    = grub-mkrescue

# -------------------- 编译参数 --------------------
CFLAGS  = -m32 -ffreestanding -nostdlib -fno-pie -Wall -Wextra -g \
          -I src/include                          # 公共头文件路径

# CFLAGS  = -m32 -ffreestanding -nostdlib -fno-pie -Wall -Wextra -g \
          -I src/include -DKERNEL_DEBUG #DEBUG

ASMFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

# -------------------- 目录结构 --------------------
SRC_DIR     = src
BUILD_DIR   = build
BOOT_DIR    = $(SRC_DIR)/boot
KERNEL_DIR  = $(SRC_DIR)/kernel
DRIVERS_DIR = $(SRC_DIR)/drivers
INCLUDE_DIR = $(SRC_DIR)/include
MEMORY_DIR = $(SRC_DIR)/memory
PROCESS_DIR = $(SRC_DIR)/process
FILESYS_DIR = $(SRC_DIR)/fs

# -------------------- 目标文件列表 --------------------
OBJS = $(BUILD_DIR)/start.o \
       $(BUILD_DIR)/kmain.o \
       $(BUILD_DIR)/screen.o \
       $(BUILD_DIR)/serial.o \
	   $(BUILD_DIR)/tty.o \
	   $(BUILD_DIR)/pmm.o \
	   $(BUILD_DIR)/vmm.o \
	   $(BUILD_DIR)/heap.o \
	   $(BUILD_DIR)/gdt.o \
	   $(BUILD_DIR)/tss.o \
	   $(BUILD_DIR)/task.o \
	   $(BUILD_DIR)/proc.o \
	   $(BUILD_DIR)/signal.o \
	   $(BUILD_DIR)/printf.o \
	   $(BUILD_DIR)/string.o \
	   $(BUILD_DIR)/idt.o \
	   $(BUILD_DIR)/isr.o \
	   $(BUILD_DIR)/volume.o \
       $(BUILD_DIR)/path.o \
	   $(BUILD_DIR)/ata.o \
	   $(BUILD_DIR)/fat32.o \
	   $(BUILD_DIR)/elf_loader.o \
	   $(BUILD_DIR)/fs.o \
	   $(BUILD_DIR)/syscall.o \
	   $(BUILD_DIR)/scheduler.o \
	   $(BUILD_DIR)/keyboard.o \
	   $(BUILD_DIR)/device.o \
	   $(BUILD_DIR)/ipc.o \
	   $(BUILD_DIR)/pci.o \
	   $(BUILD_DIR)/partition.o \
	   $(BUILD_DIR)/uaccess.o \
	   $(BUILD_DIR)/rtc.o \
	   $(BUILD_DIR)/acpi.o \
	   $(BUILD_DIR)/panic.o \
	   $(BUILD_DIR)/irq.o

	   

# -------------------- 最终产物 --------------------
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
ISO        = HeliumOS.iso
DISK_IMG   = $(BUILD_DIR)/HeliumOS.img

# ==================== 默认目标 ====================
all: $(ISO)

# ==================== 编译规则 ====================
$(BUILD_DIR)/start.o: $(BOOT_DIR)/start.asm
	@mkdir -p $(BUILD_DIR)
	$(ASM) $(ASMFLAGS) $< -o $@

# 通用规则：编译 src/kernel/ 和 src/drivers/ 下的 .c 文件
$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(DRIVERS_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(MEMORY_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(PROCESS_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(FILESYS_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $@


# ==================== ISO 镜像（默认） ====================
$(ISO): $(KERNEL_ELF)
	@mkdir -p $(BUILD_DIR)/iso_root/boot/grub
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso_root/boot/
	@echo "set timeout=5" > $(BUILD_DIR)/iso_root/boot/grub/grub.cfg
	@echo "menuentry \"HeliumOS\" {" >> $(BUILD_DIR)/iso_root/boot/grub/grub.cfg
	@echo "  multiboot2 /boot/kernel.elf" >> $(BUILD_DIR)/iso_root/boot/grub/grub.cfg
	@echo "}" >> $(BUILD_DIR)/iso_root/boot/grub/grub.cfg
	$(GRUB) -o $(ISO) $(BUILD_DIR)/iso_root > /dev/null 2>&1
	@echo "ISO 镜像生成成功: $(ISO)"

# ==================== 硬盘镜像（用于测试文件系统写入） ====================
$(DISK_IMG): $(KERNEL_ELF) $(BUILD_DIR)/SHELL.ELF $(BUILD_DIR)/IDLE.ELF $(BUILD_DIR)/TEST.ELF $(BUILD_DIR)/TESTKILL.ELF $(BUILD_DIR)/SLEEPER.ELF $(BUILD_DIR)/ARGTEST.ELF $(BUILD_DIR)/TESTWRITE.ELF $(BUILD_DIR)/TESTFAULT.ELF $(BUILD_DIR)/TESTENV.ELF $(BUILD_DIR)/COUNT.ELF $(BUILD_DIR)/CAT.ELF $(BUILD_DIR)/ECHO.ELF $(BUILD_DIR)/HEAD.ELF $(BUILD_DIR)/TAIL.ELF $(BUILD_DIR)/WC.ELF
	@echo "正在创建 FAT32 硬盘镜像 (需要 sudo 权限)..."
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=64 status=none
	(echo o; echo n; echo p; echo 1; echo 2048; echo; echo t; echo c; echo a; echo 1; echo w) | fdisk $(DISK_IMG) > /dev/null 2>&1
	@OFFSET=$$((2048*512)); \
	sudo losetup /dev/loop22 $(DISK_IMG); \
	sudo losetup /dev/loop23 $(DISK_IMG) -o $$OFFSET; \

	sudo mkfs.vfat -F 32 /dev/loop23 > /dev/null; \
	sudo mount /dev/loop23 /mnt/build; \
	sudo mkdir -p /mnt/build/boot/grub; \
	sudo cp $(KERNEL_ELF) /mnt/build/boot/; \


	sudo cp $(BUILD_DIR)/IDLE.ELF /mnt/build/IDLE; \
	sudo cp $(BUILD_DIR)/SHELL.ELF /mnt/build/SHELL; \
	sudo cp $(BUILD_DIR)/TEST.ELF /mnt/build/TEST; \
	sudo cp $(BUILD_DIR)/TESTKILL.ELF /mnt/build/TESTKILL; \
	sudo cp $(BUILD_DIR)/SLEEPER.ELF /mnt/build/SLEEPER; \
	sudo cp $(BUILD_DIR)/ARGTEST.ELF /mnt/build/ARGTEST; \
	sudo cp $(BUILD_DIR)/TESTWRITE.ELF /mnt/build/TESTWRITE; \
	sudo cp $(BUILD_DIR)/TESTFAULT.ELF /mnt/build/TESTFAULT; \
	sudo cp $(BUILD_DIR)/TESTENV.ELF /mnt/build/TESTENV; \
	sudo cp $(BUILD_DIR)/COUNT.ELF /mnt/build/COUNT; \
	sudo cp $(BUILD_DIR)/CAT.ELF /mnt/build/CAT; \
	sudo cp $(BUILD_DIR)/ECHO.ELF /mnt/build/ECHO; \
	sudo cp $(BUILD_DIR)/HEAD.ELF /mnt/build/HEAD; \
	sudo cp $(BUILD_DIR)/TAIL.ELF /mnt/build/TAIL; \
	sudo cp $(BUILD_DIR)/WC.ELF /mnt/build/WC; \

	sudo cp $(SRC_DIR)/kernel/kmain.c /mnt/build/kmain.c; \
	sudo cp user/idle.c /mnt/build/idle.c; \

	echo "set timeout=5" | sudo tee /mnt/build/boot/grub/grub.cfg > /dev/null; \
	echo "menuentry \"HeliumOS\" {" | sudo tee -a /mnt/build/boot/grub/grub.cfg > /dev/null; \
	echo "  multiboot2 /boot/kernel.elf" | sudo tee -a /mnt/build/boot/grub/grub.cfg > /dev/null; \
	echo "}" | sudo tee -a /mnt/build/boot/grub/grub.cfg > /dev/null; \
	sudo grub-install --target=i386-pc --boot-directory=/mnt/build/boot --modules="biosdisk part_msdos fat" /dev/loop22 > /dev/null 2>&1; \
	sync; \
	sudo umount /mnt/build; \
	sudo losetup -d /dev/loop23; \
	sudo losetup -d /dev/loop22
	@echo "硬盘镜像生成成功: $(DISK_IMG)"

# 	test
	dd if=/dev/zero of=$(BUILD_DIR)/TESTIMG.img bs=1M count=64 status=none
	(echo o; echo n; echo p; echo 1; echo 2048; echo; echo t; echo c; echo w) | fdisk $(BUILD_DIR)/TESTIMG.img > /dev/null 2>&1
	@OFFSET=$$((2048*512)); \
	sudo losetup /dev/loop22 $(BUILD_DIR)/TESTIMG.img; \
	sudo losetup /dev/loop23 $(BUILD_DIR)/TESTIMG.img -o $$OFFSET; \

	sudo mkfs.vfat -F 32 /dev/loop23 > /dev/null; \
	sudo losetup -d /dev/loop23; \
	sudo losetup -d /dev/loop22
	

# ==================== 运行目标 ====================
# 默认运行 ISO（省事）
run: $(ISO)
	$(QEMU) -cdrom $(ISO) -serial stdio 

# 运行硬盘镜像（测试文件系统时使用）
run-disk: $(DISK_IMG)
	$(QEMU) -drive file=$(DISK_IMG),format=raw -serial stdio -m 1024 -drive file=$(BUILD_DIR)/TESTIMG.img,format=raw

# 调试（与 run 相同，只是名称更明确）
debug: $(ISO)
	$(QEMU) -cdrom $(ISO) -serial stdio -s -S

debug-disk: $(DISK_IMG)
	$(QEMU) -drive file=$(DISK_IMG),format=raw -serial stdio -s -S

# ==================== 清理 ====================
clean:
	rm -rf $(BUILD_DIR) $(ISO) $(DISK_IMG)

gdb: $(ISO)
	gdb-multiarch -ex "target remote localhost:1234" -ex "symbol-file build/kernel.elf" build/kernel.elf

pack:
	find . -type f \( -name "*.c" -o -name "*.h" -o -name "*.asm" -o -name "*.ld" -o -name "Makefile" \) -print0 | xargs -0 -I {} sh -c 'echo "==================== {} ===================="; cat "{}"; echo ""' > project.txt





# USER BUILD
USER_CFLAGS = -m32 -ffreestanding -fno-pic -fno-stack-protector -Iuser/ -Iuser/libc/include
LIBC_SRCS = user/libc/string.c user/libc/printf.c user/libc/malloc.c
LIBC_OBJS = $(BUILD_DIR)/libc_string.o \
			$(BUILD_DIR)/libc_printf.o \
			$(BUILD_DIR)/libc_malloc.o \
			$(BUILD_DIR)/libc_signal.o \
            $(BUILD_DIR)/libc_sigreturn.o \
			$(BUILD_DIR)/libc_env.o \
			$(BUILD_DIR)/libc_assert.o \
			$(BUILD_DIR)/libc_ctype.o \
			$(BUILD_DIR)/libc_qsort.o \
			$(BUILD_DIR)/libc_abort.o \
			$(BUILD_DIR)/libc_stdlib.o 

$(BUILD_DIR)/libc_string.o: user/libc/string.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc_printf.o: user/libc/printf.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc_malloc.o: user/libc/malloc.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/crt0.o: user/crt0.asm
	@mkdir -p $(BUILD_DIR)
	$(ASM) $(ASMFLAGS) $< -o $@

$(BUILD_DIR)/libc_signal.o: user/libc/signal.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc_sigreturn.o: user/libc/sigreturn.asm
	@mkdir -p $(BUILD_DIR)
	$(ASM) $(ASMFLAGS) $< -o $@

$(BUILD_DIR)/libc_env.o: user/libc/env.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc_assert.o: user/libc/assert.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc_ctype.o: user/libc/ctype.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc_qsort.o: user/libc/qsort.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc_abort.o: user/libc/abort.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc_stdlib.o: user/libc/stdlib.c
	$(CC) $(USER_CFLAGS) -c $< -o $@


# shell
$(BUILD_DIR)/shell.o: user/shell.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/SHELL.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/shell.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/shell.o $(LIBC_OBJS)

# idle
$(BUILD_DIR)/idle.o: user/idle.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/IDLE.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/idle.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/idle.o $(LIBC_OBJS)


# test
$(BUILD_DIR)/test.o: user/test.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/TEST.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/test.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/test.o $(LIBC_OBJS)

# testkill
$(BUILD_DIR)/testkill.o: user/testkill.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/TESTKILL.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/testkill.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/testkill.o $(LIBC_OBJS)

# sleeper
$(BUILD_DIR)/sleeper.o: user/sleeper.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/SLEEPER.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/sleeper.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/sleeper.o $(LIBC_OBJS)

# argtest
$(BUILD_DIR)/argtest.o: user/argtest.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ARGTEST.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/argtest.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/argtest.o $(LIBC_OBJS)

# test_write
$(BUILD_DIR)/test_write.o: user/test_write.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/TESTWRITE.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/test_write.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/test_write.o $(LIBC_OBJS)

# test_fault
$(BUILD_DIR)/test_fault.o: user/test_fault.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/TESTFAULT.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/test_fault.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/test_fault.o $(LIBC_OBJS)

# test_env
$(BUILD_DIR)/test_env.o: user/test_env.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/TESTENV.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/test_env.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/test_env.o $(LIBC_OBJS)

# count
$(BUILD_DIR)/count.o: user/count.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/COUNT.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/count.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/count.o $(LIBC_OBJS)

# cat
$(BUILD_DIR)/cat.o: user/cat.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/CAT.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/cat.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/cat.o $(LIBC_OBJS)

# echo
$(BUILD_DIR)/echo.o: user/echo.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ECHO.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/echo.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/echo.o $(LIBC_OBJS)

# head
$(BUILD_DIR)/head.o: user/head.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/HEAD.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/head.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/head.o $(LIBC_OBJS)

# tail
$(BUILD_DIR)/tail.o: user/tail.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/TAIL.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/tail.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/tail.o $(LIBC_OBJS)

# wc
$(BUILD_DIR)/wc.o: user/wc.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/WC.ELF: $(BUILD_DIR)/crt0.o $(BUILD_DIR)/wc.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $(BUILD_DIR)/crt0.o $(BUILD_DIR)/wc.o $(LIBC_OBJS)



# 声明伪目标
.PHONY: all run run-disk debug debug-disk clean gdb pack