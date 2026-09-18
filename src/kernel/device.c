#include <device.h>
#include <task.h>
#include <errno.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>

#define DEV_MAX_TYPES  8

static const struct device_ops *dev_ops[DEV_MAX_TYPES];

void dev_init(void) {
    for (int i = 0; i < DEV_MAX_TYPES; i++) {
        dev_ops[i] = NULL;
    }
    kprintf("[DEV] Initialized\n");
}

int dev_register(int type, const struct device_ops *ops) {
    if (type <= 0 || type >= DEV_MAX_TYPES || !ops) return EINVAL;
    if (dev_ops[type]) return EBUSY;   /* 已注册 */
    dev_ops[type] = ops;
    kprintf("[DEV] Registered type %d\n", type);
    return OK;
}

/* ---------- 句柄管理 ---------- */
static dev_t dev_alloc_handle(void) {
    struct task *cur = get_current_task();
    if (!cur) return DEV_INVALID;
    for (int i = 0; i < DEV_MAX_HANDLES; i++) {
        if (!cur->dev_handles[i].used) {
            memset(&cur->dev_handles[i], 0, sizeof(struct dev_handle));
            cur->dev_handles[i].used = 1;
            return i;
        }
    }
    return DEV_INVALID;
}

static struct dev_handle *dev_get(dev_t dev) {
    struct task *cur = get_current_task();
    if (!cur || dev < 0 || dev >= DEV_MAX_HANDLES) return NULL;
    if (!cur->dev_handles[dev].used) return NULL;
    return &cur->dev_handles[dev];
}

/* ---------- 对外 API ---------- */
dev_t dev_open(int type, void *arg) {
    if (type <= 0 || type >= DEV_MAX_TYPES) return DEV_INVALID;
    if (!dev_ops[type]) return DEV_INVALID;

    dev_t dev = dev_alloc_handle();
    if (dev < 0) return DEV_INVALID;

    struct dev_handle *h = dev_get(dev);
    h->type = type;
    h->state = NULL;

    if (dev_ops[type]->open) {
        if (dev_ops[type]->open(&h->state, arg) != OK) {
            h->used = 0;
            return DEV_INVALID;
        }
    }
    return dev;
}

int dev_read(dev_t dev, void *buf, uint32_t n) {
    struct dev_handle *h = dev_get(dev);
    if (!h) return EINVAL;
    const struct device_ops *ops = dev_ops[h->type];
    if (!ops || !ops->read) return ENOSYS;
    return ops->read(h->state, buf, n);
}

int dev_write(dev_t dev, const void *buf, uint32_t n) {
    struct dev_handle *h = dev_get(dev);
    if (!h) return EINVAL;
    const struct device_ops *ops = dev_ops[h->type];
    if (!ops || !ops->write) return ENOSYS;
    return ops->write(h->state, buf, n);
}

int dev_ioctl(dev_t dev, uint32_t cmd, void *arg) {
    struct dev_handle *h = dev_get(dev);
    if (!h) return EINVAL;
    const struct device_ops *ops = dev_ops[h->type];
    if (!ops || !ops->ioctl) return ENOSYS;
    return ops->ioctl(h->state, cmd, arg);
}

int dev_close(dev_t dev) {
    struct dev_handle *h = dev_get(dev);
    if (!h) return EINVAL;
    const struct device_ops *ops = dev_ops[h->type];
    if (ops && ops->close) ops->close(h->state);
    h->used = 0;
    return OK;
}

void dev_release_all(struct task *task) {
    if (!task) return;
    for (int i = 0; i < DEV_MAX_HANDLES; i++) {
        task->dev_handles[i].used = 0;
    }
}