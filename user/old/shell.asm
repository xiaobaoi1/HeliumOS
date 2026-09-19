; ============================================================
; HeliumOS Shell (纯汇编)
; 系统调用号（与内核一致）
; ============================================================

; ---------- 系统调用号 ----------
%define SYS_EXIT       1
%define SYS_GETPID     2
%define SYS_SPAWN      3
%define SYS_WAITPID    4
%define SYS_SLEEP      5
%define SYS_READ       11
%define SYS_WRITE      12
%define SYS_BRK        21

; ---------- 宏：系统调用 ----------
%macro syscall 4
    mov eax, %1
    mov ebx, %2
    mov ecx, %3
    mov edx, %4
    int 0x80
%endmacro

; ---------- 数据段 ----------
section .data
prompt:     db '$ ', 0
newline:    db 10, 0
msg_welcome: db 'HeliumOS Shell', 10, 0
msg_help:   db 'Commands: run <file>, help, exit', 10, 0
msg_unknown: db 'Unknown command', 10, 0
msg_run_fail: db 'Failed to spawn', 10, 0
msg_exit:   db 'Goodbye!', 10, 0

cmd_buf:    times 64 db 0
cmd_len:    dd 0

; ---------- 代码段 ----------
section .text
global _start

_start:
    mov esi, msg_welcome
    call print_str

.main_loop:
    mov esi, prompt
    call print_str

    ; 读取命令行（最多 63 字符）
    mov edi, cmd_buf
    xor ebx, ebx            ; 字符计数
.read_loop:
    syscall SYS_READ, 0, edi, 1
    cmp eax, 1
    jne .read_loop
    mov al, [edi]
    cmp al, 10              ; 换行符
    je .read_done
    ; 忽略退格等控制字符（只允许可打印字符）
    cmp al, 32
    jb .read_loop
    inc edi
    inc ebx
    cmp ebx, 63
    jb .read_loop
.read_done:
    mov byte [edi], 0       ; 字符串终止
    mov [cmd_len], ebx
    ; 打印换行
    mov esi, newline
    call print_str

    ; 解析命令
    mov esi, cmd_buf
    cmp byte [esi], 0
    je .main_loop          ; 空命令

    ; ---------- help ----------
    mov edi, help_cmd
    call strcmp
    test eax, eax
    jnz .check_run
    mov esi, msg_help
    call print_str
    jmp .main_loop

.check_run:
    ; ---------- run ----------
    mov edi, run_cmd
    call strncmp           ; 比较前3个字符
    test eax, eax
    jnz .check_exit

    ; 提取文件名（跳过 "run "）
    mov esi, cmd_buf + 4
    syscall SYS_SPAWN, esi, 0, 0
    cmp eax, 0
    jge .wait_child
    mov esi, msg_run_fail
    call print_str
    jmp .main_loop
.wait_child:
    push eax
    syscall SYS_WAITPID, eax, 0, 0
    pop eax
    jmp .main_loop

.check_exit:
    ; ---------- exit ----------
    mov edi, exit_cmd
    call strcmp
    test eax, eax
    jnz .unknown
    mov esi, msg_exit
    call print_str
    syscall SYS_EXIT, 0, 0, 0

.unknown:
    mov esi, msg_unknown
    call print_str
    jmp .main_loop

; ---------- 子程序：打印字符串（ESI指向字符串） ----------
print_str:
    push eax
    push ebx
    push ecx
    push edx
    mov edx, 0
.len_loop:
    cmp byte [esi + edx], 0
    je .done
    inc edx
    jmp .len_loop
.done:
    syscall SYS_WRITE, 1, esi, edx
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ---------- 子程序：字符串比较（ESI和EDI，返回0表示相等） ----------
strcmp:
    push esi
    push edi
.loop:
    mov al, [esi]
    mov bl, [edi]
    cmp al, bl
    jne .diff
    test al, al
    jz .equal
    inc esi
    inc edi
    jmp .loop
.diff:
    mov eax, 1
    jmp .done
.equal:
    mov eax, 0
.done:
    pop edi
    pop esi
    ret

; ---------- 子程序：字符串前缀比较（比较ESI和EDI的前3个字符，返回0表示相等） ----------
strncmp:
    push esi
    push edi
    mov ecx, 3
.loop:
    mov al, [esi]
    mov bl, [edi]
    cmp al, bl
    jne .diff
    test al, al
    jz .equal
    inc esi
    inc edi
    loop .loop
.equal:
    mov eax, 0
    jmp .done
.diff:
    mov eax, 1
.done:
    pop edi
    pop esi
    ret

; ---------- 命令字符串常量 ----------
section .rodata
help_cmd:   db 'help', 0
run_cmd:    db 'run', 0
exit_cmd:   db 'exit', 0