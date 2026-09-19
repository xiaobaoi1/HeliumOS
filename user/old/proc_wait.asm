section .text
global _start

_start:
    ; 打印父进程 PID
    mov eax, 4          ; SYS_GETPID
    int 0x80
    mov [pid], eax

    mov eax, 1
    mov ebx, 1
    mov ecx, msg
    mov edx, msglen
    int 0x80

    mov eax, [pid]       ; 把pid的值加载进 eax
    call print_uint32    ; 调用打印无符号十进制数字

    mov eax, 1
    mov ebx, 1
    mov ecx, newline
    mov edx, 1
    int 0x80

    ; 返回值在 eax，我们把它作为参数给 write？
    ; 为了简单，我们直接调用 waitpid(-1, status)
    ; 等待任意子进程
    mov eax, 3          ; SYS_WAITPID
    mov ebx, -1         ; pid = -1 表示任意子进程
    mov ecx, status     ; 存放退出状态
    int 0x80

    ; 打印子进程 PID（返回值在 eax）
    ; 我们简单地把 PID 放在内存，然后打印？
    ; 为了演示，我们可以先不打印，直接退出。
    ; 或者我们打印一条消息
    mov eax, 1
    mov ebx, 1
    mov ecx, msg_done
    mov edx, msg_done_len
    int 0x80

    mov eax, 2
    mov ebx, 0
    int 0x80


;-----------------------------------------------------------
; print_uint32: 打印eax中32位无符号整数(十进制)
; 破坏寄存器: eax,ebx,ecx,edx,edi
;-----------------------------------------------------------
print_uint32:
    mov edi, buf_end     ; edi 指向缓冲区尾部
    mov ebx, 10          ; 除数 = 10
.print_loop:
    xor edx, edx         ; div要求edx清零，否则会崩溃！
    div ebx              ; eax = eax/10 , edx = eax%10
    add dl, '0'          ; 余数 0‑9 → ASCII '0'~'9'
    dec edi              ; 往左挪一格
    mov [edi], dl        ; 存字符
    test eax, eax        ; 判断商是否等于0
    jnz .print_loop      ; 商≠0就继续除

    ; edi现在就是字符串起始地址
    ; 计算字符串长度： buf_end - edi
    mov ecx, edi
    mov edx, buf_end
    sub edx, edi

    ; write 系统调用
    mov eax, 1
    mov ebx, 1
    int 0x80

    ret

section .data
status: dd 0
pid: dd 0
msg: db 'Parent: start', 10
msglen equ $ - msg
msg_done: db 'Parent: child exited', 10
msg_done_len equ $ - msg_done
newline: db 10

; 输出用缓冲区 (栈也可以，这里用内存缓冲区直观)
buf:        resb 16      ; 存放十进制字符串，预留16字节
buf_end:

