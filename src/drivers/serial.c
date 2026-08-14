#include <serial.h>
#include <io.h>

// 初始化 COM1 串口
void serial_init(void) {
    uint16_t port = SERIAL_COM1_BASE;

    // 1. 禁用所有中断（防止 QEMU 触发不需要的 IRQ）
    outb(port + SERIAL_INTERRUPT, 0x00);

    // 2. 启用 DLAB（除数锁存器访问位），准备设置波特率
    outb(port + SERIAL_LINE_CTRL, 0x80);

    // 3. 设置波特率为 115200（除数 = 115200 / 115200 = 1）
    //    低字节 = 0x01, 高字节 = 0x00
    outb(port + SERIAL_DATA, 0x01);      // 低字节
    outb(port + SERIAL_INTERRUPT, 0x00); // 高字节（注意复用中断寄存器）

    // 4. 设置线路参数：8 位数据，1 位停止位，无校验 (8N1)
    //    线路控制寄存器值 = 0x03 (位0=1: 8位数据, 位1=1: 1位停止, 位2=0: 无校验)
    outb(port + SERIAL_LINE_CTRL, 0x03);

    // 5. 启用 FIFO（清除缓冲区，设置 14 字节阈值）
    outb(port + SERIAL_FIFO, 0xC7);

    // 6. 启用调制解调器控制（DTR + RTS + 中断使能）
    outb(port + SERIAL_MODEM_CTRL, 0x0B);
}

// 检查发送保持寄存器是否为空（只有空时才能发送新数据）
int serial_is_transmit_empty(void) {
    uint16_t port = SERIAL_COM1_BASE;
    // 读取线路状态寄存器，检查第 5 位 (Transmit Holding Register Empty)
    return (inb(port + SERIAL_LINE_STATUS) & 0x20) != 0;
}

// 发送一个字符（阻塞式轮询）
void serial_write_char(char c) {
    uint16_t port = SERIAL_COM1_BASE;

    // 如果遇到换行符，先发送回车符（\r），确保终端光标回到行首
    if (c == '\n') {
        while (!serial_is_transmit_empty());
        outb(port, '\r');
    }

    // 等待发送缓冲区空
    while (!serial_is_transmit_empty());

    // 将字符写入数据寄存器
    outb(port, c);
}

// 发送字符串
void serial_write_string(const char *str) {
    if (!str) return;
    for (int i = 0; str[i] != '\0'; i++) {
        serial_write_char(str[i]);
    }
}