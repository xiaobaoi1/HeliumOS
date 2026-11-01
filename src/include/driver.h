#ifndef DRIVER_H
#define DRIVER_H

/* 驱动初始化节。
 *
 * 驱动定义 `int xxx_init(void)`，用 DRIVER_INIT(xxx_init) 注册：
 *   - 返回 0：驱动就绪
 *   - 返回非 0：硬件不存在 / 依赖未满足，跳过
 *
 * 所有注册函数在 kmain 里 pci_init / net_init 之后、net_proto_init 之前
 * 遍历调用。顺序是链接顺序（大致是 OBJS 顺序），驱动间不应有隐式依赖。
 *
 * 不需要遍历的早期依赖（serial / screen / tty / dev / irq / keyboard）
 * 仍保持 kmain 里直接调用。 */

typedef int (*driver_init_fn)(void);

#define DRIVER_INIT(fn) \
    static driver_init_fn __driver_init_##fn \
        __attribute__((used, section(".driver_init"))) = (fn)

#endif