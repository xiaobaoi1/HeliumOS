#include <screen.h>
#include <io.h>
#include <stddef.h>
#include <device.h>
#include <errno.h>

#define VGA_MEMORY   ((uint16_t*)0xB8000)

#define VGA_CTRL_REG   0x3D4
#define VGA_DATA_REG   0x3D5
#define VGA_CURSOR_HI  0x0E
#define VGA_CURSOR_LO  0x0F

static uint16_t *vga_buffer = VGA_MEMORY;

/* 保存硬件光标位置（GET_HW_CURSOR 需要读回） */
static int hw_cursor_x = 0;
static int hw_cursor_y = 0;

/* ---------- 内部硬件操作 ---------- */

static void hw_set_cursor(int x, int y) {
    uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);
    outb(VGA_CTRL_REG, VGA_CURSOR_HI);
    outb(VGA_DATA_REG, (pos >> 8) & 0xFF);
    outb(VGA_CTRL_REG, VGA_CURSOR_LO);
    outb(VGA_DATA_REG, pos & 0xFF);
    hw_cursor_x = x;
    hw_cursor_y = y;
}

/* ---------- device_ops 实现 ---------- */

static int vga_open(void **state, void *arg) {
    (void)arg;
    *state = NULL;   /* VGA dev 无 per-handle 状态 */
    return OK;
}

static int vga_write(void *state, const void *buf, uint32_t n) {
    (void)state;
    if (!buf || n == 0) return EINVAL;
    if (n % sizeof(struct vga_cell) != 0) return EINVAL;

    const struct vga_cell *cells = (const struct vga_cell*)buf;
    uint32_t count = n / sizeof(struct vga_cell);

    for (uint32_t i = 0; i < count; i++) {
        int x = cells[i].x;
        int y = cells[i].y;
        if (x < 0 || x >= VGA_WIDTH) continue;
        if (y < 0 || y >= VGA_HEIGHT) continue;
        vga_buffer[y * VGA_WIDTH + x] =
            ((uint16_t)cells[i].attr << 8) | (uint8_t)cells[i].c;
    }
    return (int)n;
}

static int vga_read(void *state, void *buf, uint32_t n) {
    (void)state; (void)buf; (void)n;
    return ENOSYS;   /* 读 cell 暂未实现 */
}

static int vga_ioctl(void *state, uint32_t cmd, void *arg) {
    (void)state;
    switch (cmd) {
        case VGA_IOCTL_CLEAR: {
            uint8_t attr = (uint8_t)(uint32_t)arg;
            uint16_t cell = ((uint16_t)attr << 8) | ' ';
            for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
                vga_buffer[i] = cell;
            }
            return OK;
        }
        case VGA_IOCTL_SET_HW_CURSOR: {
            int x = (int)(((uint32_t)arg >> 16) & 0xFFFF);
            int y = (int)((uint32_t)arg & 0xFFFF);
            if (x < 0 || x >= VGA_WIDTH) return EINVAL;
            if (y < 0 || y >= VGA_HEIGHT) return EINVAL;
            hw_set_cursor(x, y);
            return OK;
        }
        case VGA_IOCTL_GET_HW_CURSOR: {
            int *out = (int*)arg;
            if (!out) return EINVAL;
            out[0] = hw_cursor_x;
            out[1] = hw_cursor_y;
            return OK;
        }
        case VGA_IOCTL_SCROLL: {
            uint8_t attr = (uint8_t)(uint32_t)arg;
            /* 上移一行 */
            for (int y = 1; y < VGA_HEIGHT; y++) {
                for (int x = 0; x < VGA_WIDTH; x++) {
                    vga_buffer[(y - 1) * VGA_WIDTH + x] =
                        vga_buffer[y * VGA_WIDTH + x];
                }
            }
            /* 清空最后一行 */
            uint16_t cell = ((uint16_t)attr << 8) | ' ';
            for (int x = 0; x < VGA_WIDTH; x++) {
                vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = cell;
            }
            return OK;
        }
        default:
            return EINVAL;
    }
}

static void vga_close(void *state) {
    (void)state;
}

static const struct device_ops vga_ops = {
    .open  = vga_open,
    .read  = vga_read,
    .write = vga_write,
    .ioctl = vga_ioctl,
    .close = vga_close,
};

/* ---------- 初始化 ---------- */

void screen_init(void) {
    /* 清屏为默认色 */
    uint16_t cell = ((uint16_t)VGA_DEFAULT_ATTR << 8) | ' ';
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = cell;
    }
    hw_set_cursor(0, 0);

    /* 注册为 dev */
    dev_register(DEV_TYPE_VGA, &vga_ops);
}