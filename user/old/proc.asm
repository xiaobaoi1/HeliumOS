section .text
global _start

_start:
    jmp _start

    ; exit(0)
    mov eax, 2
    mov ebx, 0
    int 0x80

section .data
msg: db 'P1: Hello', 10
msglen equ $ - msg