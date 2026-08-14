#include "screen.h"
#include "serial.h"
#include <stdint.h>

// 声明 GRUB 传入的参数（虽未用，但保留避免编译警告）
extern uint32_t multiboot2_info_addr;

void kmain(uint32_t magic, uint32_t addr) {
    // 1. 初始化硬件设备
    screen_init();      // VGA 文本模式
    serial_init();      // COM1 串口（生命线）

    // 2. 同时向两个设备输出
    const char *msg = "Hello from HeliumOS!\nSerial output works!\n";
    
    screen_write_string(msg);
    serial_write_string(msg);

    // 3. 也可以额外输出带数字的调试信息
    serial_write_string("Kernel loaded at physical address 0x100000.\n");

    // 死循环（后续会被调度器取代）
    while (1) {
        // 暂时用 CPU 暂停指令省电（但必须开启中断才有效，这里先空转）
        __asm__ volatile("hlt");
    }
}