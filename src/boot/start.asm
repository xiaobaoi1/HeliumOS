; 启动汇编：身份映射低 4MB，开启分页，原地进入 kmain

%define PAGE_SIZE 4096

section .multiboot
align 4
    dd 0xE85250D6
    dd 0
    dd header_end - header_start
    dd 0x100000000 - (0xE85250D6 + 0 + (header_end - header_start))

header_start:
    dw 0
    dw 0
    dd 8
header_end:

section .text
global _start
extern kmain

_start:
    mov [magic_phys], eax
    mov [addr_phys], ebx

    ; 临时栈
    mov esp, temp_stack_top

    ; 建立身份映射页表（0~4MB）
    mov edi, page_directory
    mov esi, page_table_identity

    ; 清零页目录
    mov ecx, PAGE_SIZE / 4
    xor eax, eax
    rep stosd

    ; 填充页表（1024 项）
    mov edi, esi
    xor eax, eax
    mov ecx, 1024
.identity_loop:
    or eax, 0x003          ; Present + Write
    stosd
    add eax, PAGE_SIZE
    loop .identity_loop

    ; 页目录第 0 项指向页表
    mov edi, page_directory
    mov eax, esi
    or eax, 0x003
    mov [edi], eax

    ; 开启分页
    mov eax, page_directory
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; 无需远跳转，原地继续（身份映射生效）

    ; 设置正式栈（高一些，避开 .bss）
    mov esp, stack_top

    push dword [addr_phys]
    push dword [magic_phys]
    call kmain

    cli
    hlt
    jmp $

; ============================================================
; 中断处理入口（由IDT调用）
; ============================================================

section .text

; 宏：定义异常入口（无错误码） 
%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push byte 0
    push byte %1
    jmp isr_common_stub
%endmacro

; 宏：定义异常入口（有错误码） 
%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    push byte %1
    jmp isr_common_stub
%endmacro

; 定义 0-31 号异常 
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_NOERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; IRQ 定义 (0-15) 
%macro IRQ 2
global irq%1
irq%1:
    cli
    push byte 0
    push byte %2
    jmp irq_common_stub
%endmacro

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; 通用异常处理（调用C函数 isr_handler） 
extern isr_handler
isr_common_stub:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10          ; 内核数据段
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp              ; 传递寄存器结构指针
    call isr_handler
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8            ; 清除错误码和中断号
    iret

; 通用IRQ处理（调用C函数 irq_handler） 
extern irq_handler
irq_common_stub:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp
    call irq_handler
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

section .data
global isr_entry_table
isr_entry_table:
    dd isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dd isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dd isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dd isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

global irq_entry_table
irq_entry_table:
    dd irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    dd irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

section .data
align 4
magic_phys: dd 0
addr_phys:  dd 0

section .bss
align 4096
global page_directory
page_directory:     resb 4096
global page_table_identity
page_table_identity: resb 4096

temp_stack_bottom:
    resb 4096
temp_stack_top:

stack_bottom:
    resb 16384
stack_top: