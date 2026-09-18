; user/syscall.asm
; 系统调用入口，供 C 语言调用
; 调用约定：cdecl
;   int syscall(int num, int arg1, int arg2, int arg3);
;   num = eax, arg1 = ebx, arg2 = ecx, arg3 = edx
;   返回值 = eax

global syscall

section .text
syscall:
    ; 保存 ebp（cdecl 标准）
    push ebp
    mov ebp, esp

    ; 取参数（cdecl 参数从 [ebp+8] 开始）
    mov eax, [ebp + 8]   ; num
    mov ebx, [ebp + 12]  ; arg1
    mov ecx, [ebp + 16]  ; arg2
    mov edx, [ebp + 20]  ; arg3

    ; 触发系统调用
    int 0x80

    ; 恢复 ebp
    pop ebp
    ret