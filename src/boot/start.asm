section .multiboot
align 4
    dd 0xE85250D6                 ; 魔数 (Multiboot2)
    dd 0                          ; 架构 (0 = i386)
    dd header_end - header_start  ; 头部长度
    dd 0x100000000 - (0xE85250D6 + 0 + (header_end - header_start)) ; 校验和

header_start:
    ; 结束标签 (必须)
    dw 0                          ; 类型
    dw 0                          ; 标志
    dd 8                          ; 长度
header_end:

section .text
global _start
extern kmain

_start:
    ; 设置栈指针 (使用平坦地址，栈在 .bss 段后)
    mov esp, stack_top

    ; 按照 cdecl 调用约定，参数从右向左压栈
    push ebx    ; 压入 multiboot2_info 地址 (第二个参数，addr)
    push eax    ; 压入 magic (第一个参数，magic)
    call kmain

    ; 如果 kmain 返回，死循环
    cli
    hlt
    jmp $

section .bss
align 16
stack_bottom:
    resb 16384  ; 16 KB 栈
stack_top: