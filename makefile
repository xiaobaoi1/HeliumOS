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
          -I src/include -MMD -MP                          # 公共头文件路径

# CFLAGS  = -m32 -ffreestanding -nostdlib -fno-pie -Wall -Wextra -g \
          -I src/include -DKERNEL_DEBUG -MMD -MP #DEBUG

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
	   $(BUILD_DIR)/irq.o \
	   $(BUILD_DIR)/uaccess_asm.o \
	   $(BUILD_DIR)/extable.o \
	   $(BUILD_DIR)/vma.o \
	   $(BUILD_DIR)/shm.o \
	   $(BUILD_DIR)/net.o \
	   $(BUILD_DIR)/rtl8139.o \
	   $(BUILD_DIR)/net_proto.o \
	   $(BUILD_DIR)/net_sock.o \
	   $(BUILD_DIR)/tcp.o \
	   $(BUILD_DIR)/kmap.o \
	   $(BUILD_DIR)/fb.o \
	   $(BUILD_DIR)/font.o \
	   $(BUILD_DIR)/display.o \
	   $(BUILD_DIR)/vga_text.o \
	   $(BUILD_DIR)/fb_char.o \
	   $(BUILD_DIR)/mouse.o \
	   $(BUILD_DIR)/input.o \
	   $(BUILD_DIR)/pty.o \

	   


# -------------------- 最终产物 --------------------
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
ISO        = HeliumOS.iso
DISK_IMG   = $(BUILD_DIR)/HeliumOS.img

# ==================== 默认目标 ====================
all: $(DISK_IMG)




# ==================== 用户程序变量 ====================

USER_CFLAGS = -m32 -ffreestanding -fno-pic -fno-stack-protector \
              -Iuser/ -Iuser/libc/include -MMD -MP

USER_APPS = shell idle test testkill sleeper argtest test_write \
            test_fault test_env count cat echo head tail wc \
            test_mmap test_stack ping test_dns nc tcpserver \
            test_input test_pty wm term test_namedpipe test_perm \
			test_createwin test_ansi

USER_OBJS = $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(USER_APPS)))
USER_ELFS = $(addprefix $(BUILD_DIR)/,$(addsuffix .ELF,$(USER_APPS)))

LIBC_SRCS = string printf malloc signal env assert ctype qsort \
            abort stdlib inet netdb

LIBC_OBJS = $(addprefix $(BUILD_DIR)/libc_,$(addsuffix .o,$(LIBC_SRCS))) \
            $(BUILD_DIR)/libc_sigreturn.o


# ==================== 编译规则 ====================
$(BUILD_DIR)/start.o: $(BOOT_DIR)/start.asm
	@mkdir -p $(BUILD_DIR)
	$(ASM) $(ASMFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.asm
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
$(DISK_IMG): $(KERNEL_ELF) $(USER_ELFS)
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
	sudo cp tools/unifont.hex /mnt/build/UNICODE.HEX; \
	sudo cp tools/unifont-17.0.03.hex /mnt/build/UNICODE-17.HEX; \
	for app in $(USER_APPS); do \
	    sudo cp $(BUILD_DIR)/$$app.ELF /mnt/build/$$app; \
	done; \
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
# 	dd if=/dev/zero of=$(BUILD_DIR)/TESTIMG.img bs=1M count=64 status=none
# 	(echo o; echo n; echo p; echo 1; echo 2048; echo; echo t; echo c; echo w) | fdisk $(BUILD_DIR)/TESTIMG.img > /dev/null 2>&1
# 	@OFFSET=$$((2048*512)); \
# 	sudo losetup /dev/loop22 $(BUILD_DIR)/TESTIMG.img; \
# 	sudo losetup /dev/loop23 $(BUILD_DIR)/TESTIMG.img -o $$OFFSET; \

# 	sudo mkfs.vfat -F 32 /dev/loop23 > /dev/null; \
# 	sudo losetup -d /dev/loop23; \
# 	sudo losetup -d /dev/loop22
	

# ==================== 运行目标 ====================
# 默认运行 ISO（省事）
run: $(ISO)
	$(QEMU) -cdrom $(ISO) -serial stdio 

# 运行硬盘镜像（测试文件系统时使用）
run-disk: $(DISK_IMG)
	$(QEMU) -drive file=$(DISK_IMG),format=raw -serial stdio -m 1024 \
	        -netdev user,id=n0 -device rtl8139,netdev=n0

run-tcp: $(DISK_IMG)
	$(QEMU) -drive file=$(DISK_IMG),format=raw -serial stdio -m 1024 \
	        -netdev user,id=n0,hostfwd=tcp::12345-:12345 \
	        -device rtl8139,netdev=n0

# debug
debug-disk: $(DISK_IMG)
	$(QEMU) -drive file=$(DISK_IMG),format=raw -serial stdio -m 1024 \
	        -netdev user,id=n0 -device rtl8139,netdev=n0 -s -S
# ==================== 清理 ====================
clean:
	rm -rf $(BUILD_DIR) $(ISO) $(DISK_IMG)

gdb: $(ISO)
	gdb-multiarch -ex "target remote localhost:1234" -ex "symbol-file build/kernel.elf" build/kernel.elf

pack:
	find . -type f \( -name "*.c" -o -name "*.h" -o -name "*.asm" -o -name "*.ld" -o -name "Makefile" \) -print0 | xargs -0 -I {} sh -c 'echo "==================== {} ===================="; cat "{}"; echo ""' > project.txt



# ==================== 用户程序 ====================

# ---------- 编译 ----------

$(BUILD_DIR)/crt0.o: user/crt0.asm
	@mkdir -p $(BUILD_DIR)
	$(ASM) $(ASMFLAGS) $< -o $@

$(BUILD_DIR)/libc_sigreturn.o: user/libc/sigreturn.asm
	@mkdir -p $(BUILD_DIR)
	$(ASM) $(ASMFLAGS) $< -o $@

$(BUILD_DIR)/libc_%.o: user/libc/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: user/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# ---------- 链接 ----------

$(BUILD_DIR)/%.ELF: $(BUILD_DIR)/%.o $(BUILD_DIR)/crt0.o $(LIBC_OBJS) user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ \
		$(BUILD_DIR)/crt0.o $< $(LIBC_OBJS)

# 声明伪目标
.PHONY: all run run-disk debug debug-disk clean gdb pack run-net run-tcp $(DISK_IMG)
-include $(wildcard $(BUILD_DIR)/*.d)