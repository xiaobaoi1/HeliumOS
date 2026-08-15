section .text
global _start

_start:
    ; 调用 SYS_WRITE (eax=1)
    ; write(1, msg, len)
    mov eax, 1
    mov ebx, 1
    mov ecx, msg
    mov edx, msglen
    int 0x80

    ; 调用 SYS_EXIT (eax=2)
    mov eax, 2
    mov ebx, 1
    int 0x80

section .data
msg: db 'Hello from user process!', 10   ; 10 = \n
msglen equ $ - msg