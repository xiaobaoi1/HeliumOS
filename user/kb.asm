section .text
global _start

_start:
    ; write_str("=== Keyboard Test ===\n")
    mov eax, 12          ; SYS_WRITE
    mov ebx, 1           ; stdout
    mov ecx, msg1
    mov edx, msg1_len
    int 0x80

    ; write_str("Press any key (ESC to exit)\n")
    mov eax, 12
    mov ebx, 1
    mov ecx, msg2
    mov edx, msg2_len
    int 0x80

.loop:
    ; read_char(&c)
    mov eax, 11          ; SYS_READ
    mov ebx, 0           ; stdin
    mov ecx, buffer
    mov edx, 1
    int 0x80

    ; if (eax == 1) { ... }
    cmp eax, 1
    jne .loop

    ; c = buffer[0]
    movzx ecx, byte [buffer]

    ; if (c == 27) break (ESC)
    cmp ecx, 27
    je .exit

    ; write_char(c)
    ; 直接输出字符（缓冲区长度为 1）
    mov eax, 12
    mov ebx, 1
    mov ecx, buffer
    mov edx, 1
    int 0x80

    jmp .loop

.exit:
    ; write_str("\nESC pressed, exiting.\n")
    mov eax, 12
    mov ebx, 1
    mov ecx, msg3
    mov edx, msg3_len
    int 0x80

    ; exit(0)
    mov eax, 1           ; SYS_EXIT
    mov ebx, 0
    int 0x80

section .data
msg1 db '=== Keyboard Test ===', 10
msg1_len equ $ - msg1
msg2 db 'Press any key (ESC to exit)', 10
msg2_len equ $ - msg2
msg3 db 10, 'ESC pressed, exiting.', 10
msg3_len equ $ - msg3

section .bss
buffer resb 1