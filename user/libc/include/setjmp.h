#ifndef USER_SETJMP_H
#define USER_SETJMP_H

/* i386 SysV jmp_buf 布局：保存 ebx, esi, edi, ebp, esp, eip */
typedef unsigned int jmp_buf[6];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif