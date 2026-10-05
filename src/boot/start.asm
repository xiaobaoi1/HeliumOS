; ====================================================================
; 启动汇编：Multiboot2 头，身份映射，开启分页，进入 kmain
; 同时包含所有中断入口（异常、IRQ、系统调用）
; ====================================================================

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
extern isr_handler
extern irq_handler
extern isr_syscall_handler
extern tss_set_kernel_stack

_start:
    mov [magic_phys], eax
    mov [addr_phys], ebx

    mov esp, temp_stack_top

    cld

    ; 拷贝 Multiboot2 info 到 bss（分页前，物理地址直接可访问）
    ; GRUB 可能把 info 放在任意物理地址，拷贝后只访问 bss
    mov esi, ebx
    mov edi, mb_info_buf
    mov ecx, [esi]              ; total_size（首字段）
    cmp ecx, 16384
    jbe .copy_ok
    mov ecx, 16384
.copy_ok:
    rep movsb
    mov [addr_phys], dword mb_info_buf

    ; 清零 page_directory（4KB）
    mov edi, page_directory
    mov ecx, PAGE_SIZE / 4
    xor eax, eax
    rep stosd

    ; PDE 0..31：恒等映射物理 0..128MB
    mov edi, page_directory
    mov eax, 0x83
    mov ecx, 32
.fill_low:
    stosd
    add eax, 0x400000
    loop .fill_low

    ; PDE 32..63：kmap 窗口（虚拟 0x08000000..0x10000000）
    ; 内核运行时按需填 4KB 页表
    ; PDE 64..255：未映射

    ; 打开 CR4.PSE
    mov eax, cr4
    or  eax, 0x10               ; PSE = bit 4
    mov cr4, eax

    ; 开启分页
    mov eax, page_directory
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; 设置正式栈
    mov esp, stack_top

    push dword [addr_phys]
    push dword [magic_phys]
    call kmain

    cli
    hlt
    jmp $

; ============================================================
; 中断入口
; ============================================================

; 宏：定义异常入口（无错误码）
%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push 0
    push %1
    jmp isr_common_stub
%endmacro

; 宏：定义异常入口（有错误码）
%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    push %1
    jmp isr_common_stub
%endmacro

; 0-31 号异常
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

; IRQ 入口 (32-47)
%macro IRQ 2
global irq%1
irq%1:
    cli
    push 0
    push %2
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

; ===== 系统调用入口 (int 0x80) =====
global isr80
isr80:
    cli
    push 0
    push 0x80
    jmp syscall_common_stub

; ===== 通用异常处理 =====
isr_common_stub:
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
    call isr_handler
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

; ===== 通用 IRQ 处理 =====
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

; ===== 系统调用处理 (调用 C 函数) =====
syscall_common_stub:
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
    call isr_syscall_handler
    add esp, 4
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

; ============================================================
; 上下文切换：switch_to(prev, next)
; 参数：cdecl 约定，[esp+4]=prev, [esp+8]=next
; ============================================================
global switch_to
switch_to:
    push ebp
    push ebx
    push esi
    push edi

    ; 保存 prev 的 esp（如果 prev 非 NULL）
    mov eax, [esp + 20]          ; prev
    test eax, eax
    jz .no_save
    mov [eax + 20], esp          ; prev->kernel_esp = esp
.no_save:

    ; 取 next
    mov eax, [esp + 24]          ; next

    ; 更新 TSS.esp0（在切栈前，用当前栈）
    mov ecx, [eax + 16]          ; next->kernel_stack_phys
    add ecx, 4096                ; 栈顶
    push eax                     ; 保存 next
    push ecx                     ; 参数
    call tss_set_kernel_stack
    add esp, 4
    pop eax                      ; 恢复 next

    ; 切 CR3
    mov ecx, [eax + 12]          ; next->pgd
    mov cr3, ecx

    ; 切到 next 的栈
    mov esp, [eax + 20]          ; next->kernel_esp

    ; 恢复 callee-saved
    pop edi
    pop esi
    pop ebx
    pop ebp

    ; 返回：首次调度会跳到 enter_user_mode；
    ; 后续切换会回到 schedule 调用 switch_to 之后
    ret

global enter_user_mode
enter_user_mode:
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret

; ============================================================
; 数据段：中断入口表（供 C 使用）
; ============================================================
section .data
global isr_entry_table
isr_entry_table:
    ; 32 个异常入口
    dd isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dd isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dd isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dd isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

global irq_entry_table
irq_entry_table:
    ; 16 个 IRQ 入口
    dd irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    dd irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

; 系统调用入口单独导出（在 idt.c 中引用）
global isr80

; ============================================================
; BSS 段
; ============================================================
section .bss
align 4096
global page_directory
page_directory:     resb 4096

temp_stack_bottom:
    resb 4096
temp_stack_top:

stack_bottom:
    resb 16384
stack_top:

mb_info_buf:
    resb 16384

section .data
align 4
magic_phys: dd 0
addr_phys:  dd 0