; user/crt0.asm
; 用户态启动桩：从栈上读 argc/argv，调用 _start(argc, argv)
; _start 返回后执行 SYS_EXIT

section .text
global _start_crt0
extern _start

_start_crt0:
    xor ebp, ebp                ; 清空帧指针，方便调试

    mov eax, [esp]              ; argc
    lea ebx, [esp + 4]          ; &argv[0]

    push ebx                    ; 第二参数
    push eax                    ; 第一参数
    call _start

    ; _start 返回 eax = 退出码
    mov ebx, eax
    mov eax, 1                  ; SYS_EXIT
    int 0x80

    hlt                         ; 不会到达