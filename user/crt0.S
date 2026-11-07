.globl _start_crt0
.globl _start
.extern main
.extern __libc_init_environ

_start:
_start_crt0:
    xor %ebp, %ebp

    # 提取栈上的 argc, argv, envp
    mov (%esp), %eax            # eax = argc
    lea 4(%esp), %edx           # edx = argv
    lea 8(%esp, %eax, 4), %ecx  # ecx = envp (esp + 8 + 4*argc)

    # 保存 argc, argv, envp 到栈上
    push %ecx
    push %edx
    push %eax

    # 调用 __libc_init_environ(envp)
    push %ecx
    call __libc_init_environ
    add $4, %esp

    # 恢复 argc, argv
    pop %eax
    pop %edx
    pop %ecx

    # 调用 main(argc, argv)
    push %edx
    push %eax
    call main
    add $8, %esp

    # exit 系统调用
    mov %eax, %ebx
    mov $1, %eax
    int $0x80
    hlt