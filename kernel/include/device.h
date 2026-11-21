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
/* 释放一个进程的所有设备句柄。
 *
 * 遍历每个 used 句柄，调用驱动 close 回调（如有），然后标 used=0。
 *
 * 契约：
 *   - 驱动的 open 负责分配自己的资源，close 负责释放
 *   - close 必须幂等、可重入、不依赖调用顺序
 *   - close 里不得调用任何依赖 current_task 的接口
 *   - 驱动不在 open 里分配需要 close 释放的资源也可以，close 留空即可
 */
void dev_release_all(struct task *task);

/* ---------- 驱动注册 ---------- */
int  dev_register(int type, const struct device_ops *ops);

/* 内核态拿 ops 的接口（不分配 per-process 句柄）
 * 用于内核组件（tty、fs 的底层等）直接访问设备能力。
 * 返回 NULL 表示未注册。 */
const struct device_ops *dev_get_ops(int type);

/* ---------- 设备 API ---------- */
dev_t dev_open(int type, void *arg);
int   dev_read(dev_t dev, void *buf, uint32_t n);
int   dev_write(dev_t dev, const void *buf, uint32_t n);
int   dev_ioctl(dev_t dev, uint32_t cmd, void *arg);
int   dev_close(dev_t dev);

#endif