#include <syscall.h>
#include <printf.h>
#include <serial.h>
#include <task.h>
#include <stdint.h>

/* 系统调用：write(int fd, const char *buf, size_t count) */
static int sys_write(int fd, const char *buf, uint32_t count) {
    if (fd != 1) {  /* 仅支持 stdout */
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        serial_write_char(buf[i]);
    }
    return count;
}

uint32_t get_current_pid(void) {
    /* 简化：因为当前只有单进程，直接返回 1 */
    /* 未来实现多进程时，需要从 TSS 或全局变量获取 */
    return get_current_task()->pid;
}

/* 系统调用：exit(int status) */
static void sys_exit(int status) {
    kprintf("[SYSCALL] Process %d exited with status %d\n", 
            get_current_pid(), status);
    
    struct task *current = get_current_task();
    if (current) {
        task_exit(current);   // 释放资源
    }
    
    // 切换到下一个进程
    schedule();
    
    // 如果 schedule 返回（没有其他进程），进入空闲循环
    while (1) __asm__("cli; hlt");
}

/* 系统调用分发器（由中断处理函数调用） */
void syscall_handler(struct registers *regs) {
    /* 参数：eax = 调用号，ebx/ecx/edx = 参数 */
    uint32_t syscall_no = regs->eax;
    uint32_t arg1 = regs->ebx;
    uint32_t arg2 = regs->ecx;
    uint32_t arg3 = regs->edx;

    int32_t ret = 0;

    switch (syscall_no) {
        case SYS_WRITE:
            ret = sys_write((int)arg1, (const char*)arg2, (uint32_t)arg3);
            break;
        case SYS_EXIT:
            sys_exit((int)arg1);
            break;
        default:
            kprintf("[SYSCALL] Unknown syscall %d\n", syscall_no);
            ret = -1;
    }

    /* 返回值放在 eax 中（由调用者通过寄存器读取） */
    regs->eax = ret;
}

// /* 获取当前进程 PID（供 sys_exit 使用） */
// uint32_t get_current_pid(void) {
//     /* 简化：因为当前只有单进程，直接返回 1 */
//     /* 未来实现多进程时，需要从 TSS 或全局变量获取 */
//     return 1;
// }