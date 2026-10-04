; ====================================================================
; 用户内存拷贝：CPU 走 MMU，fault 由 __ex_table 恢复。
; nasm Intel 语法。
; ====================================================================

section .text

; int __copy_user(void *dst, uint32_t src, uint32_t n, int to_user)
;   cdecl: [esp+4]=dst, [esp+8]=src, [esp+12]=n, [esp+16]=to_user
;   返回 0 成功，非 0 失败。
global __copy_user
__copy_user:
    push ebx
    push esi
    push edi

    mov edi, [esp + 16]     ; dst
    mov esi, [esp + 20]     ; src
    mov ecx, [esp + 24]     ; n
    mov ebx, [esp + 28]     ; to_user

    test ecx, ecx
    jz __copy_user_success

    test ebx, ebx
    jnz __copy_user_to_user

__copy_user_from_user:
__uaccess_fu_read:
    mov al, [esi]
__uaccess_fu_write:
    mov [edi], al
    inc esi
    inc edi
    dec ecx
    jnz __copy_user_from_user
    jmp __copy_user_success

__copy_user_to_user:
__uaccess_tu_read:
    mov al, [esi]
__uaccess_tu_write:
    mov [edi], al
    inc esi
    inc edi
    dec ecx
    jnz __copy_user_to_user

__copy_user_success:
    xor eax, eax
    pop edi
    pop esi
    pop ebx
    ret

; fault 恢复：返回值非 0，恢复 callee-saved，直接返回。
global __copy_user_fixup
__copy_user_fixup:
    mov eax, 1
    pop edi
    pop esi
    pop ebx
    ret

section __ex_table progbits alloc noexec nowrite
    dd __uaccess_fu_read,  __copy_user_fixup
    dd __uaccess_fu_write, __copy_user_fixup
    dd __uaccess_tu_read,  __copy_user_fixup
    dd __uaccess_tu_write, __copy_user_fixup