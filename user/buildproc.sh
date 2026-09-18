nasm -f elf32 user/proc.asm -o user/proc1.o
ld -m elf_i386 -T user/linker.ld -o user/proc.elf user/proc1.o
# nasm -f elf32 user/proc2.asm -o user/proc2.o
# ld -m elf_i386 -T user/linker.ld -o user/proc2.elf user/proc2.o
# nasm -f elf32 user/proc_wait.asm -o user/proc3.o
# ld -m elf_i386 -T user/linker.ld -o user/proc3.elf user/proc3.o
gcc -m32 -nostdlib -fno-builtin -static -mno-red-zone -fno-stack-protector -c user/test_brk.c -o user/test_brk.o
ld -m elf_i386 -T user/linker.ld user/test_brk.o -o user/test_brk.elf

gcc -m32 -nostdlib -fno-builtin -static -mno-red-zone -fno-stack-protector -fno-omit-frame-pointer -ffreestanding -O0 -c user/test_keyboard.c -o user/test_keyboard.o
ld -m elf_i386 -T user/linker.ld user/test_keyboard.o -o user/test_keyboard.elf

gcc -m32 -nostdlib -fno-builtin -static -mno-red-zone -fno-stack-protector -c user/idle.c -o user/idle.o
ld -m elf_i386 -T user/linker.ld user/idle.o -o user/idle.elf

nasm -f elf32 user/kb.asm -o user/kb.o
ld -m elf_i386 -T user/linker.ld -o user/kb.elf user/kb.o

# asm syscall
nasm -f elf32 user/syscall.asm -o user/syscall.o

gcc -m32 -nostdlib -fno-builtin -static -mno-red-zone -fno-stack-protector -c user/shell.c -o user/shell.o
ld -m elf_i386 -T user/linker.ld -o user/shell.elf user/shell.o user/syscall.o

gcc -m32 -nostdlib -fno-builtin -static -mno-red-zone -fno-stack-protector -c user/test_fs.c -o user/test_fs.o
ld -m elf_i386 -T user/linker.ld -o user/test_fs.elf user/test_fs.o user/syscall.o

gcc -m32 -nostdlib -fno-builtin -static -mno-red-zone -fno-stack-protector -c user/test_dev.c -o user/test_dev.o
ld -m elf_i386 -T user/linker.ld -o user/test_dev.elf user/test_dev.o user/syscall.o

gcc -m32 -nostdlib -fno-builtin -static -mno-red-zone -fno-stack-protector -c user/test_console.c -o user/test_console.o
ld -m elf_i386 -T user/linker.ld -o user/test_console.elf user/test_console.o user/syscall.o


# nasm -f elf32 user/shell.asm -o user/shell.o
# ld -m elf_i386 -T user/linker.ld -o user/shell.elf user/shell.o
