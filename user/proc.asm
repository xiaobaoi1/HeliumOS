section .text
global _start

_start:
    mov ecx, 10
.loop:
    push ecx
    mov eax, 1          ; SYS_WRITE
    mov ebx, 1
    mov ecx, msg
    mov edx, msglen
    int 0x80
    pop ecx
    dec ecx
    jnz .loop

    mov eax, 2          ; SYS_EXIT
    mov ebx, 0
    int 0x80

section .data
msg: db 'Hello from user process!', 10
msglen equ $ - msg