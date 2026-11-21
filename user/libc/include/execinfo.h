#ifndef USER_EXECINFO_H
#define USER_EXECINFO_H

/* GNU 扩展。HeliumOS libc 不实现栈回溯——需要 libunwind 和 ELF 符号表，
 * 超出 libc 范围。NetSurf 只在 assert 失败时用，stub 返回 0 是合法最小实现。 */

int backtrace(void **buffer, int size);
void backtrace_symbols_fd(void *const *buffer, int size, int fd);
char **backtrace_symbols(void *const *buffer, int size);

#endif