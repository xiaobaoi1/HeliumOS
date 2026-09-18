#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

/* 句柄类型与限制 */
typedef int dev_t;
#define DEV_MAX_HANDLES  8
#define DEV_INVALID     (-1)

/* 设备类型 */
#define DEV_TYPE_NONE      0
#define DEV_TYPE_KEYBOARD  1
#define DEV_TYPE_SERIAL    2
#define DEV_TYPE_VGA       3
#define DEV_TYPE_ATA       4

/* 设备操作表：每个驱动实现一套 */
struct device_ops {
    /* 打开设备，state 由驱动填充（可为 NULL） */
    int  (*open)(void **state, void *arg);
    /* 读设备，返回读取字节数（可为 0 表示无数据） */
    int  (*read)(void *state, void *buf, uint32_t n);
    /* 写设备，返回写入字节数 */
    int  (*write)(void *state, const void *buf, uint32_t n);
    /* 控制命令，具体语义由驱动定义 */
    int  (*ioctl)(void *state, uint32_t cmd, void *arg);
    /* 关闭设备 */
    void (*close)(void *state);
};

/* 每进程的句柄槽位 */
struct dev_handle {
    uint8_t  used;
    uint8_t  type;          /* DEV_TYPE_* */
    uint16_t reserved;
    void    *state;         /* 驱动私有状态 */
};

/* 前向声明，避免循环 */
struct task;

/* ---------- 生命周期 ---------- */
void dev_init(void);
void dev_release_all(struct task *task);

/* ---------- 驱动注册 ---------- */
int  dev_register(int type, const struct device_ops *ops);

/* ---------- 设备 API ---------- */
dev_t dev_open(int type, void *arg);
int   dev_read(dev_t dev, void *buf, uint32_t n);
int   dev_write(dev_t dev, const void *buf, uint32_t n);
int   dev_ioctl(dev_t dev, uint32_t cmd, void *arg);
int   dev_close(dev_t dev);

#endif