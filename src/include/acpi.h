#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* sys_reboot 的命令参数 */
#define ACPI_CMD_HALT    0
#define ACPI_CMD_REBOOT  1

/* 扫描 Multiboot2 info（优先）+ 内存（fallback），
 * 找到 RSDP → FADT，填内部状态。失败时打印日志，不 panic。 */
void acpi_init(uint32_t multiboot_info_addr);

/* 关机。FADT 未就绪时返回 ENOSYS。 */
int acpi_poweroff(void);

/* 重启。ACPI RESET_REG 优先，fallback 8042。 */
int acpi_reboot(void);

#endif