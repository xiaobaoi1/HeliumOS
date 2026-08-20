section .text
global _start

_start:
    mov ecx, 5          ; 循环次数
.loop:
    push ecx
    ; write(1, msg, len)
    mov eax, 1
    mov ebx, 1
    mov ecx, msg
    mov edx, msglen
    int 0x80
    pop ecx
    dec ecx
    jnz .loop

    ; exit(0)
    mov eax, 2
    mov ebx, 0
    int 0x80

section .data
msg: db 'P1: Hello', 10
msglen equ $ - msg