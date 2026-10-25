section .text
global setjmp
global longjmp

; int setjmp(jmp_buf env)
setjmp:
    mov eax, [esp + 4]      ; env 指针
    mov [eax], ebx
    mov [eax + 4], esi
    mov [eax + 8], edi
    mov [eax + 12], ebp
    lea ecx, [esp + 4]      ; 保存当前 esp（跳过返回地址）
    mov [eax + 16], ecx
    mov ecx, [esp]          ; 保存返回地址（eip）
    mov [eax + 20], ecx
    xor eax, eax            ; 返回 0
    ret

; void longjmp(jmp_buf env, int val)
longjmp:
    mov eax, [esp + 4]      ; env 指针
    mov edx, [esp + 8]      ; val
    mov ebx, [eax]
    mov esi, [eax + 4]
    mov edi, [eax + 8]
    mov ebp, [eax + 12]
    mov esp, [eax + 16]
    mov ecx, [eax + 20]     ; 恢复 eip
    test edx, edx           ; 若 val 为 0，则设为 1
    jnz .ret
    inc edx
.ret:
    mov eax, edx
    jmp ecx

global _setjmp
_setjmp:
    jmp setjmp