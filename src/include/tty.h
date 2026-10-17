#ifndef TTY_H
#define TTY_H

#include <stdint.h>

/* tty 只负责内核 bootstrap 显示：
 *   kprintf → tty_write_char → cell_buf → display
 * 用户进程 I/O 走 pty，不经过 tty。没有 SYS_TTY_* 系统调用。 */

void tty_init(void);
void tty_try_upgrade(void);

/* kprintf 用：逐字符输出 */
void tty_write_char(char c);

/* 批量写（内部用） */
int  tty_write(const char *buf, uint32_t n);

#endif