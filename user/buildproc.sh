nasm -f elf32 user/proc.asm -o user/proc1.o
ld -m elf_i386 -T user/linker.ld -o user/proc.elf user/proc1.o
nasm -f elf32 user/proc2.asm -o user/proc2.o
ld -m elf_i386 -T user/linker.ld -o user/proc2.elf user/proc2.o