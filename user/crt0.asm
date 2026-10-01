section .text
global _start_crt0
extern main
extern __libc_init_environ

_start_crt0:
    xor ebp, ebp

    mov esi, [esp]              ; argc
    lea edi, [esp + 4]          ; argv

    ; envp = &argv[argc+1] = esp + 8 + 4*argc
    lea ecx, [esp + 8 + esi*4]

    ; 初始化 libc 环境
    push ecx
    call __libc_init_environ
    add esp, 4

    ; 调 main(argc, argv)
    push edi
    push esi
    call main

    ; main 返回 eax = 退出码
    mov ebx, eax
    mov eax, 1
    int 0x80
    hlt