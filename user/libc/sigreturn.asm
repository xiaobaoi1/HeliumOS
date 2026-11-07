section .text
global __sigreturn_trampoline
__sigreturn_trampoline:
    mov eax, 72
    int 0x80
    hlt